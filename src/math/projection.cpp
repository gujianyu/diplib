/*
 * DIPlib 3.0
 * This file contains the definition for the various projection functions.
 *
 * (c)2017, Cris Luengo.
 * Based on original DIPlib code: (c)1995-2014, Delft University of Technology.
 */

#include <cmath>

#include "diplib.h"
#include "diplib/math.h"
#include "diplib/framework.h"
#include "diplib/overload.h"
#include <diplib/iterators.h>

#include "../library/copy_buffer.h"


namespace dip {


namespace {

class ProjectionScanFunction {
   public:
      // The filter to be applied to each sub-image, which fills out a single sample in `out`. The `out` pointer
      // must be cast to the requested `outImageType` in the call to `ProjectionScan`.
      virtual void Project( Image const& in, Image const& mask, void* out ) = 0;
      // A virtual destructor guarantees that we can destroy a derived class by a pointer to base
      virtual ~ProjectionScanFunction() {}
};

void ProjectionScan(
      Image const& c_in,
      Image const& c_mask,
      Image& c_out,
      DataType outImageType,
      BooleanArray process,   // taken by copy so we can modify
      ProjectionScanFunction& function
) {
   UnsignedArray inSizes = c_in.Sizes();
   dip::uint nDims = inSizes.size();

   // Check inputs
   if( process.empty() ) {
      // An empty process array means all dimensions are to be processed
      process.resize( nDims, true );
   } else {
      DIP_THROW_IF( process.size() != nDims, E::ARRAY_PARAMETER_WRONG_LENGTH );
   }

   // Make simplified copy of input image header so we can modify it at will.
   // This also effectively separates input and output images. They still point
   // at the same data, but we can strip the output image without destroying
   // the input pixel data.
   Image input = c_in.QuickCopy();
   PixelSize pixelSize = c_in.PixelSize();
   String colorSpace = c_in.ColorSpace();
   Tensor outTensor = c_in.Tensor();

   // Check mask, expand mask singleton dimensions if necessary
   Image mask;
   bool hasMask = false;
   if( c_mask.IsForged() ) {
      mask = mask.QuickCopy();
      DIP_START_STACK_TRACE
         mask.CheckIsMask( inSizes, Option::AllowSingletonExpansion::DO_ALLOW, Option::ThrowException::DO_THROW );
         mask.ExpandSingletonDimensions( inSizes );
      DIP_END_STACK_TRACE
      hasMask = true;
   }

   // Determine output sizes
   UnsignedArray outSizes = inSizes;
   UnsignedArray procSizes = inSizes;
   for( dip::uint ii = 0; ii < nDims; ++ii ) {
      if( inSizes[ ii ] == 1 ) {
         process[ ii ] = false;
      }
      if( process[ ii ] ) {
         outSizes[ ii ] = 1;
      } else {
         procSizes[ ii ] = 1;
      }
   }

   // Is there anything to do?
   if( !process.any() ) {
      //std::cout << "Projection framework: nothing to do!" << std::endl;
      c_out = c_in; // This ignores the mask image
      return;
   }

   // Adjust output if necessary (and possible)
   DIP_START_STACK_TRACE
      if( c_out.IsForged() && ( c_out.IsOverlappingView( input ) || ( hasMask && c_out.IsOverlappingView( mask )))) {
         c_out.Strip();
      }
      c_out.ReForge( outSizes, outTensor.Elements(), outImageType, Option::AcceptDataTypeChange::DO_ALLOW );
      // NOTE: Don't use c_in any more from here on. It has possibly been reforged!
      c_out.ReshapeTensor( outTensor );
   DIP_END_STACK_TRACE
   c_out.SetPixelSize( pixelSize );
   c_out.SetColorSpace( colorSpace );
   Image output = c_out.QuickCopy();

   // Do tensor to spatial dimension if necessary
   if( outTensor.Elements() > 1 ) {
      input.TensorToSpatial( 0 );
      if( hasMask ) {
         mask.TensorToSpatial( 0 );
      }
      output.TensorToSpatial( 0 );
      process.insert( 0, false );
      outSizes = output.Sizes(); // == outSizes.insert( 0, outTensor.Elements() );
      procSizes.insert( 0, 1 );
   }

   // Do we need to loop at all?
   if( process.all() ) {
      //std::cout << "Projection framework: no need to loop!" << std::endl;
      if( output.DataType() != outImageType ) {
         Image outBuffer( {}, 1, outImageType );
         function.Project( input, mask, outBuffer.Origin() );
         CopyBuffer( outBuffer.Origin(), outBuffer.DataType(), 1, 1,
                     output.Origin(), output.DataType(), 1, 1, 1, 1 );
      } else {
         function.Project( input, mask, output.Origin() );
      }
      return;
   }

   // Can we treat the images as if they were 1D?
   // TODO: This is an opportunity for improving performance if the non-processing dimensions in in, mask and out have the same layout and simple stride

   // TODO: Determine the number of threads we'll be using. The size of the data has an influence. As is the number of sub-images that we can generate

   // TODO: Start threads, each thread makes its own temp image.

   // Create view over input image, that spans the processing dimensions
   Image tempIn;
   tempIn.CopyProperties( input );
   tempIn.SetSizes( procSizes );
   tempIn.dip__SetOrigin( input.Origin() );
   tempIn.Squeeze(); // we want to make sure that function.Project() won't be looping over singleton dimensions
   // TODO: instead of Squeeze, do a "flatten as much as possible" function. But Mask must be flattened in the same way.
   // Create view over mask image, identically to input
   Image tempMask;
   if( hasMask ) {
      tempMask.CopyProperties( mask );
      tempMask.SetSizes( procSizes );
      tempMask.dip__SetOrigin( mask.Origin() );
      tempMask.Squeeze(); // keep in sync with tempIn.
   }
   // Create view over output image that doesn't contain the processing dimensions or other singleton dimensions
   Image tempOut;
   tempOut.CopyProperties( output );
   // Squeeze tempOut, but keep inStride, maskStride, outStride and outSizes in synch
   IntegerArray inStride = input.Strides();
   IntegerArray maskStride( nDims );
   if( hasMask ) {
      maskStride = mask.Strides();
   }
   IntegerArray outStride = output.Strides();
   dip::uint jj = 0;
   for( dip::uint ii = 0; ii < nDims; ++ii ) {
      if( outSizes[ ii ] > 1 ) {
         inStride[ jj ] = inStride[ ii ];
         maskStride[ jj ] = maskStride[ ii ];
         outStride[ jj ] = outStride[ ii ];
         outSizes[ jj ] = outSizes[ ii ];
         ++jj;
      }
   }
   inStride.resize( jj );
   maskStride.resize( jj );
   outStride.resize( jj );
   outSizes.resize( jj );
   nDims = jj;
   tempOut.SetSizes( outSizes );
   tempOut.dip__SetOrigin( output.Origin() );
   // Create a temporary output buffer, to collect a single sample in the data type requested by the calling function
   bool useOutputBuffer = false;
   Image outBuffer;
   if( output.DataType() != outImageType ) {
      // We need a temporary space for the output sample also, because `function.Project` expects `outImageType`.
      outBuffer.SetDataType( outImageType );
      outBuffer.Forge(); // By default it's a single sample.
      useOutputBuffer = true;
   }

   // Iterate over the pixels in the output image. For each, we create a view in the input image.
   UnsignedArray position( nDims, 0 );
   for( ;; ) {

      // Do the thing
      if( useOutputBuffer ) {
         function.Project( tempIn, tempMask, outBuffer.Origin() );
         // Copy data from output buffer to output image
         CopyBuffer( outBuffer.Origin(), outBuffer.DataType(), 1, 1,
                     tempOut.Origin(), tempOut.DataType(), 1, 1, 1, 1 );
      } else {
         function.Project( tempIn, tempMask, tempOut.Origin() );
      }

      // Next output pixel
      dip::uint dd;
      for( dd = 0; dd < nDims; dd++ ) {
         ++position[ dd ];
         tempIn.dip__ShiftOrigin( inStride[ dd ]);
         if( hasMask ) {
            tempMask.dip__ShiftOrigin( maskStride[ dd ]);
         }
         tempOut.dip__ShiftOrigin( outStride[ dd ]);
         // Check whether we reached the last pixel of the line
         if( position[ dd ] != outSizes[ dd ] ) {
            break;
         }
         // Rewind along this dimension
         tempIn.dip__ShiftOrigin( -inStride[ dd ] * position[ dd ] );
         if( hasMask ) {
            tempMask.dip__ShiftOrigin( -maskStride[ dd ] * position[ dd ] );
         }
         tempOut.dip__ShiftOrigin( -outStride[ dd ] * position[ dd ] );
         position[ dd ] = 0;
         // Continue loop to increment along next dimension
      }
      if( dd == nDims ) {
         break;            // We're done!
      }
   }

   // TODO: End threads.
}

} // namespace


namespace {

template< typename TPI >
class ProjectionMean : public ProjectionScanFunction {
   public:
      ProjectionMean( bool computeMean ) : computeMean_( computeMean ) {}
      void Project( Image const& in, Image const& mask, void* out ) override {
         dip::uint n = 0;
         FlexType< TPI > sum = 0;
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  sum += it.In();
                  ++n;
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               sum += *it;
            } while( ++it );
            n = in.NumberOfPixels();
         }
         *static_cast< FlexType< TPI >* >( out ) = ( computeMean_ && ( n > 0 ))
                                                   ? ( sum / static_cast< FloatType< TPI >>( n ))
                                                   : ( sum );
      }
   private:
      bool computeMean_ = true;
};

template< typename TPI >
ComplexType< TPI > AngleToVector( TPI v ) { return { static_cast< FloatType< TPI >>( std::cos( v )),
                                                     static_cast< FloatType< TPI >>( std::sin( v )) }; }


template< typename TPI >
class ProjectionMeanDirectional : public ProjectionScanFunction {
   public:
      void Project( Image const& in, Image const& mask, void* out ) override {
         ComplexType< TPI > sum = { 0, 0 };
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  sum += AngleToVector( it.In() );
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               sum += AngleToVector( *it );
            } while( ++it );
         }
         *static_cast< FloatType< TPI >* >( out ) = std::arg( sum ); // Is the same as FlexType< TPI > because TPI is not complex here.
      }
};

} // namespace

void Mean(
      Image const& in,
      Image const& mask,
      Image& out,
      String const& mode,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( mode == "directional" ) {
      DIP_OVL_NEW_FLOAT( lineFilter, ProjectionMeanDirectional, (), in.DataType() );
   } else {
      DIP_OVL_NEW_ALL( lineFilter, ProjectionMean, ( true ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFlex( in.DataType() ), process, *lineFilter );
}

void Sum(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   DIP_OVL_NEW_ALL( lineFilter, ProjectionMean, ( false ), in.DataType() );
   ProjectionScan( in, mask, out, DataType::SuggestFlex( in.DataType() ), process, *lineFilter );
}

namespace {

template< typename TPI >
class ProjectionProduct : public ProjectionScanFunction {
   public:
      void Project( Image const& in, Image const& mask, void* out ) override {
         FlexType< TPI > product = 1.0;
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  product *= it.In();
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               product *= *it;
            } while( ++it );
         }
         *static_cast< FlexType< TPI >* >( out ) = product;
      }
};

}

void Product(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   DIP_OVL_NEW_ALL( lineFilter, ProjectionProduct, (), in.DataType() );
   ProjectionScan( in, mask, out, DataType::SuggestFlex( in.DataType() ), process, *lineFilter );
}

namespace {

template< typename TPI >
class ProjectionMeanAbs : public ProjectionScanFunction {
   public:
      ProjectionMeanAbs( bool computeMean ) : computeMean_( computeMean ) {}
      void Project( Image const& in, Image const& mask, void* out ) override {
         dip::uint n = 0;
         FloatType< TPI > sum = 0;
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  sum += std::abs( it.In() );
                  ++n;
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               sum += std::abs( *it );
            } while( ++it );
            n = in.NumberOfPixels();
         }
         *static_cast< FloatType< TPI >* >( out ) = ( computeMean_ && ( n > 0 ))
                                                    ? ( sum / static_cast< FloatType< TPI >>( n ))
                                                    : ( sum );
      }
   private:
      bool computeMean_ = true;
};

}

void MeanAbs(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( in.DataType().IsUnsigned() ) {
      DIP_OVL_NEW_UNSIGNED( lineFilter, ProjectionMean, ( true ), in.DataType() );
   } else {
      DIP_OVL_NEW_SIGNED( lineFilter, ProjectionMeanAbs, ( true ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFloat( in.DataType() ), process, *lineFilter );
}

void SumAbs(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( in.DataType().IsUnsigned() ) {
      DIP_OVL_NEW_UNSIGNED( lineFilter, ProjectionMean, ( false ), in.DataType() );
   } else {
      DIP_OVL_NEW_SIGNED( lineFilter, ProjectionMeanAbs, ( false ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFloat( in.DataType() ), process, *lineFilter );
}

namespace {

template< typename TPI >
class ProjectionMeanSquare : public ProjectionScanFunction {
   public:
      ProjectionMeanSquare( bool computeMean ) : computeMean_( computeMean ) {}
      void Project( Image const& in, Image const& mask, void* out ) override {
         dip::uint n = 0;
         FlexType< TPI > sum = 0;
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  TPI v = it.In();
                  sum += v * v;
                  ++n;
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               TPI v = *it;
               sum += v * v;
            } while( ++it );
            n = in.NumberOfPixels();
         }
         *static_cast< FlexType< TPI >* >( out ) = ( computeMean_ && ( n > 0 ))
                                                   ? ( sum / static_cast< FloatType< TPI >>( n ))
                                                   : ( sum );
      }
   private:
      bool computeMean_ = true;
};

}

void MeanSquare(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( in.DataType().IsBinary() ) {
      DIP_OVL_NEW_BINARY( lineFilter, ProjectionMean, ( true ), DT_BIN );
   } else {
      DIP_OVL_NEW_NONBINARY( lineFilter, ProjectionMeanSquare, ( true ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFlex( in.DataType() ), process, *lineFilter );
}

void SumSquare(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( in.DataType().IsBinary() ) {
      DIP_OVL_NEW_BINARY( lineFilter, ProjectionMean, ( false ), DT_BIN );
   } else {
      DIP_OVL_NEW_NONBINARY( lineFilter, ProjectionMeanSquare, ( false ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFlex( in.DataType() ), process, *lineFilter );
}

namespace {

template< typename TPI >
class ProjectionVariance : public ProjectionScanFunction {
   public:
      ProjectionVariance( bool computeStD ) : computeStD_( computeStD ) {}
      void Project( Image const& in, Image const& mask, void* out ) override {
         VarianceAccumulator acc;
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  acc.Push( it.In() );
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               acc.Push( *it );
            } while( ++it );
         }
         *static_cast< FloatType< TPI >* >( out ) = clamp_cast< FloatType< TPI >>( computeStD_
                                                                                   ? acc.StandardDeviation()
                                                                                   : acc.Variance() );
      }
   private:
      bool computeStD_ = true;
};

template< typename TPI >
class ProjectionVarianceDirectional : public ProjectionScanFunction {
   public:
      ProjectionVarianceDirectional( bool computeStD ) : computeStD_( computeStD ) {}
      void Project( Image const& in, Image const& mask, void* out ) override {
         dip::uint n = 0;
         ComplexType< TPI > sum = { 0, 0 };
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  sum += AngleToVector( it.In() );
                  ++n;
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               sum += AngleToVector( *it );
            } while( ++it );
            n = in.NumberOfPixels();
         }
         FloatType< TPI > R = std::abs( sum );
         *static_cast< FloatType< TPI >* >( out ) = computeStD_
                                                    ? std::sqrt( FloatType< TPI >( -2.0 ) * std::log( R ))
                                                    : FloatType< TPI >( 1.0 ) - R;
      }
   private:
      bool computeStD_ = true;
};

}

void Variance(
      Image const& in,
      Image const& mask,
      Image& out,
      String const& mode,
      BooleanArray process
) {
   // TODO: This exists also for complex numbers, yielding a real value
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( mode == "directional" ) {
      DIP_OVL_NEW_FLOAT( lineFilter, ProjectionVarianceDirectional, ( false ), in.DataType() );
   } else {
      DIP_OVL_NEW_NONCOMPLEX( lineFilter, ProjectionVariance, ( false ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFloat( in.DataType() ), process, *lineFilter );
}

void StandardDeviation(
      Image const& in,
      Image const& mask,
      Image& out,
      String const& mode,
      BooleanArray process
) {
   // TODO: This exists also for complex numbers, yielding a real value
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   if( mode == "directional" ) {
      DIP_OVL_NEW_FLOAT( lineFilter, ProjectionVarianceDirectional, ( true ), in.DataType() );
   } else {
      DIP_OVL_NEW_NONCOMPLEX( lineFilter, ProjectionVariance, ( true ), in.DataType() );
   }
   ProjectionScan( in, mask, out, DataType::SuggestFloat( in.DataType() ), process, *lineFilter );
}

namespace {

template< typename TPI >
class ProjectionMaximum : public ProjectionScanFunction {
   public:
      void Project( Image const& in, Image const& mask, void* out ) override {
         TPI max = std::numeric_limits< TPI >::lowest();
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  max = std::max( max, it.In() );
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               max = std::max( max, *it );
            } while( ++it );
         }
         *static_cast< TPI* >( out ) = max;
      }
};

}

void Maximum(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   DIP_OVL_NEW_NONCOMPLEX( lineFilter, ProjectionMaximum, (), in.DataType() );
   ProjectionScan( in, mask, out, in.DataType(), process, *lineFilter );
}

namespace {

template< typename TPI >
class ProjectionMinimum : public ProjectionScanFunction {
   public:
      void Project( Image const& in, Image const& mask, void* out ) override {
         TPI min = std::numeric_limits< TPI >::max();
         if( mask.IsForged() ) {
            JointImageIterator< TPI, bin > it( in, mask );
            do {
               if( it.Out() ) {
                  min = std::min( min, it.In() );
               }
            } while( ++it );
         } else {
            ImageIterator< TPI > it( in );
            do {
               min = std::min( min, *it );
            } while( ++it );
         }
         *static_cast< TPI* >( out ) = min;
      }
};

}

void Minimum(
      Image const& in,
      Image const& mask,
      Image& out,
      BooleanArray process
) {
   std::unique_ptr< ProjectionScanFunction > lineFilter;
   DIP_OVL_NEW_NONCOMPLEX( lineFilter, ProjectionMinimum, (), in.DataType() );
   ProjectionScan( in, mask, out, in.DataType(), process, *lineFilter );
}

void Percentile(
      Image const& in,
      Image const& mask,
      Image& out,
      dfloat percentile,
      BooleanArray process
) {
   if( percentile == 0.0 ) {
      Minimum( in, mask, out, process );
   } else if( percentile == 100.0 ) {
      Maximum( in, mask, out, process );
   } else {

   }
}


} // namespace dip


#ifdef DIP__ENABLE_DOCTEST

DOCTEST_TEST_CASE("[DIPlib] testing the projection functions") {
   // We mostly test that the ProjectionScan framework works appropriately.
   // Whether the computations are performed correctly is trivial to verify during use.
   dip::Image img{ dip::UnsignedArray{ 3, 4, 2 }, 3, dip::DT_UINT8 };
   img = { 1, 1, 1 };
   img.At( 0, 0, 0 ) = { 2, 3, 4 };

   // Project over all dimensions except the tensor dimension
   dip::Image out = dip::Maximum( img );
   DOCTEST_CHECK( out.DataType() == dip::DT_UINT8 );
   DOCTEST_CHECK( out.Dimensionality() == 3 );
   DOCTEST_CHECK( out.NumberOfPixels() == 1 );
   DOCTEST_CHECK( out.TensorElements() == 3 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ] ) == 2 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ] ) == 3 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ] ) == 4 );

   // Project over two dimensions
   dip::BooleanArray ps( 3, true );
   ps[ 0 ] = false;
   out = dip::Maximum( img, {}, ps );
   DOCTEST_CHECK( out.Dimensionality() == 3 );
   DOCTEST_CHECK( out.NumberOfPixels() == 3 );
   DOCTEST_CHECK( out.Size( 0 ) == 3 );
   DOCTEST_CHECK( out.TensorElements() == 3 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 0, 0, 0 ) ) == 2 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 0, 0, 0 ) ) == 3 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 0, 0, 0 ) ) == 4 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 1, 0, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 1, 0, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 1, 0, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 2, 0, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 2, 0, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 2, 0, 0 ) ) == 1 );

   // Project over another two dimensions
   ps[ 0 ] = true;
   ps[ 1 ] = false;
   out = dip::Maximum( img, {}, ps );
   DOCTEST_CHECK( out.Dimensionality() == 3 );
   DOCTEST_CHECK( out.NumberOfPixels() == 4 );
   DOCTEST_CHECK( out.Size( 1 ) == 4 );
   DOCTEST_CHECK( out.TensorElements() == 3 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 0, 0, 0 ) ) == 2 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 0, 0, 0 ) ) == 3 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 0, 0, 0 ) ) == 4 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 0, 1, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 0, 1, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 0, 1, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 0, 2, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 0, 2, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 0, 2, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 0 ].At( 0, 3, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 1 ].At( 0, 3, 0 ) ) == 1 );
   DOCTEST_CHECK( static_cast< dip::sint >( out[ 2 ].At( 0, 3, 0 ) ) == 1 );

   // No looping at all, we project over all dimensions and have no tensor dimension
   img = dip::Image{ dip::UnsignedArray{ 3, 4, 2 }, 1, dip::DT_SFLOAT };
   img = 0;
   img.At( 0, 0, 0 ) = 1;
   out = dip::Mean( img );
   DOCTEST_CHECK( out.DataType() == dip::DT_SFLOAT );
   DOCTEST_CHECK( out.Dimensionality() == 3 );
   DOCTEST_CHECK( out.NumberOfPixels() == 1 );
   DOCTEST_CHECK( out.TensorElements() == 1 );
   DOCTEST_CHECK( static_cast< dip::dfloat >( out ) == doctest::Approx( 1.0 / ( 3.0 * 4.0 * 2.0 )));
   out = dip::Mean( img, {}, "directional" );
   DOCTEST_CHECK( out.DataType() == dip::DT_SFLOAT );
   DOCTEST_CHECK( out.Dimensionality() == 3 );
   DOCTEST_CHECK( out.NumberOfPixels() == 1 );
   DOCTEST_CHECK( out.TensorElements() == 1 );
   DOCTEST_CHECK( static_cast< dip::dfloat >( out ) == doctest::Approx( std::atan2( std::sin( 1 ),
                                                                                    std::cos( 1 ) + ( 3 * 4 * 2 - 1 ))));
}

#endif // DIP__ENABLE_DOCTEST