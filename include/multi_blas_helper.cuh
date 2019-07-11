#include <register_traits.h>

namespace quda
{

  namespace blas
  {

#define BLAS_SPINOR // do not include ghost functions in Spinor class to reduce parameter space overhead
#include <texture.h>

    // storage for matrix coefficients
#define MAX_MATRIX_SIZE 8192
#define MAX_ARG_SIZE 4096
    static __constant__ signed char Amatrix_d[MAX_MATRIX_SIZE];
    static __constant__ signed char Bmatrix_d[MAX_MATRIX_SIZE];
    static __constant__ signed char Cmatrix_d[MAX_MATRIX_SIZE];

    static signed char *Amatrix_h;
    static signed char *Bmatrix_h;
    static signed char *Cmatrix_h;

#ifdef CONSTANT_ARG
    // as a performance work around we put the argument struct into
    // __constant__ memory to prevent the compiler from spilling
    // registers on older CUDA
    static __constant__ signed char arg_buffer[MAX_ARG_SIZE];
#endif

    /**
       @brief Return the maximum power of two enabled by default for
       multi-blas.  We set a lower limit for multi-reductions, since
       we can just transpose the inner product for free, and a high
       NXZ unroll for multi-reductions lead to poor performance due to
       register spilling.
       @return Max power of two
    */
    template <bool reducer> inline constexpr int max_NXZ_power2() { return reducer ? 16 : 64; }

    template <typename T> inline constexpr bool is_power2(T x)
    {
      return (x != 0) && ((x & (x - 1)) == 0);
    }

    /**
       @brief Return if the requested nxz parameter is valid or
       not.  E.g., a valid power of two, or is less than the the
       MAX_MULTI_BLAS_N parameter.
       @param[in] nxz Requested nxz parameter
       @return True if valid, false if not
     */
    template <bool reducer> inline bool is_valid_NXZ(int nxz)
    {
      if (nxz <= MAX_MULTI_BLAS_N || // all values below MAX_MULTI_BLAS_N are valid
          ( is_power2(nxz) && nxz <= max_NXZ_power2<reducer>()) ) {
        return true;
      } else {
        return false;
      }
    }

    /**
       @brief Helper function to compute the maximum YW size for the
       multi-blas runctions.  Since the SpinorX and SpinorZ arrays are
       statically allocated with length NXZ, we can statically compute how
       the maximum size of YW is and allocate this amount of space.  This
       allows for a much larger NXZ (NYW) when NYW (NXZ) is small.
    */
    template <int NXZ, typename xType, typename yType, typename Functor> inline constexpr int max_YW_size()
    {
      using SpinorX = SpinorTexture<typename mapper<xType>::type,xType,6>;
      using SpinorY = Spinor<typename mapper<yType>::type,yType,6,1>;
      using SpinorZ = SpinorX;
      using SpinorW = Spinor<typename mapper<xType>::type,xType,6,1>;

      // compute the size remaining for the Y and W accessors
      constexpr int arg_size = (MAX_ARG_SIZE
                                - sizeof(int)          // NYW parameter
                                - sizeof(SpinorX[NXZ]) // SpinorX array
                                - (Functor::use_z ? sizeof(SpinorZ[NXZ]) : sizeof(SpinorZ*)) // SpinorZ array
                                - sizeof(int)          // functor NYW member
                                - sizeof(int)          // length parameter
                                - (!Functor::use_w ? sizeof(SpinorW*) : 0) // subtract pointer if not using W
                                - (Functor::reducer ? 3 * sizeof(void*) : 0) // reduction buffers
                                - 16)                  // there seems to be 16 bytes other argument space we need
        / ( sizeof(SpinorY) + (Functor::use_w ? sizeof(SpinorW) : 0) );

      // this is the maximum size limit imposed by the coefficient arrays
      constexpr int coeff_size = MAX_MATRIX_SIZE / (NXZ * sizeof(typename Functor::type));

      return std::min(arg_size, coeff_size);
    }

    /**
       @brief Helper function to compute the maximum YW size for the
       multi-blas runctions.  Since the SpinorX and SpinorZ arrays are
       statically allocated with length NXZ, we can statically compute how
       the maximum size of YW is and allocate this amount of space.  This
       allows for a much larger NXZ (NYW) when NYW (NXZ) is small.
    */
    inline int max_YW_size(int NXZ, QudaPrecision x_prec, QudaPrecision y_prec, size_t scalar_size,
                           bool use_z, bool use_w, bool reduce)
    {
      // ensure NXZ is a valid size
      NXZ = (reduce ? is_valid_NXZ<true>(NXZ) : is_valid_NXZ<false>(NXZ)) ?  NXZ : MAX_MULTI_BLAS_N;

      size_t spinor_x_size = x_prec < QUDA_SINGLE_PRECISION ? sizeof(SpinorTexture<float4,short4,6>) :
        sizeof(SpinorTexture<float4,float4,6>);
      size_t spinor_y_size = y_prec < QUDA_SINGLE_PRECISION ? sizeof(Spinor<float4,short4,6,1>) :
        sizeof(Spinor<float4,float4,6,1>);
      size_t spinor_z_size = spinor_x_size;
      size_t spinor_w_size = x_prec < QUDA_SINGLE_PRECISION ? sizeof(Spinor<float4,short4,6,1>) :
        sizeof(Spinor<float4,float4,6,1>);

      // compute the size remaining for the Y and W accessors
      int arg_size = (MAX_ARG_SIZE
                      - sizeof(int)         // NYW parameter
                      - NXZ*spinor_x_size // SpinorX array
                      - (use_z ? NXZ*spinor_z_size : sizeof(void*)) // SpinorZ array (else dummy pointer)
                      - sizeof(int)         // functor NYW member
                      - sizeof(int)         // length parameter
                      - (!use_w ? sizeof(void*) : 0) // subtract dummy pointer if not using W
                      - (reduce ? 3 * sizeof(void*) : 0) // reduction buffers
                      - 16)                  // there seems to be 16 bytes other argument space we need
        / ( spinor_y_size + (use_w ? spinor_w_size : 0) );

      // this is the maximum size limit imposed by the coefficient arrays
      int coeff_size = MAX_MATRIX_SIZE / (NXZ * scalar_size);

      return std::min(arg_size, coeff_size);
    }

    template <int NXZ, typename SpinorX, typename SpinorZ, bool> struct SpinorXZ
    {
      SpinorX X[NXZ];
      SpinorZ *Z;
      SpinorXZ() : Z(reinterpret_cast<SpinorZ*>(X)) { }
    };

    template <int NXZ, typename SpinorX, typename SpinorZ> struct SpinorXZ<NXZ,SpinorX,SpinorZ,true>
    {
      SpinorX X[NXZ];
      SpinorZ Z[NXZ];
    };

    template <int NYW, typename SpinorY, typename SpinorW, bool> struct SpinorYW
    {
      SpinorY Y[NYW];
      SpinorW *W;
      SpinorYW() : W(reinterpret_cast<SpinorW*>(Y)) { }
    };

    template <int NYW, typename SpinorY, typename SpinorW> struct SpinorYW<NYW,SpinorY,SpinorW,true>
    {
      SpinorY Y[NYW];
      SpinorW W[NYW];
    };

  } // namespace blas

} // namespace quda
