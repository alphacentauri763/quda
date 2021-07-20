#include <tune_quda.h>
#include <transfer.h>
#include <color_spinor_field.h>
#include <gauge_field.h>

#include <jitify_helper.cuh>

// For naive Kahler-Dirac coarsening
#include <kernels/staggered_coarse_op_kernel.cuh>

// This define controls which kernels get compiled in `coarse_op.cuh`.
// This ensures only kernels relevant for coarsening the staggered
// operator get built, saving compile time.
#define STAGGEREDCOARSE
#include <coarse_op.cuh>

namespace quda {

  /**
     @brief dummyClover is a helper function to allow us to create an
     empty clover object - this allows us to use the the externally
     linked reduction kernels when we do have a clover field. Taken from
     coarsecoarse_op.cu.
   */
  inline std::unique_ptr<cudaCloverField> dummyClover()
  {
    CloverFieldParam cf_param;
    cf_param.nDim = 4;
    cf_param.pad = 0;
    cf_param.setPrecision(QUDA_SINGLE_PRECISION);

    for (int i = 0; i < cf_param.nDim; i++) cf_param.x[i] = 0;

    cf_param.direct = true;
    cf_param.inverse = true;
    cf_param.clover = nullptr;
    cf_param.norm = 0;
    cf_param.cloverInv = nullptr;
    cf_param.invNorm = 0;
    cf_param.create = QUDA_NULL_FIELD_CREATE;
    cf_param.siteSubset = QUDA_FULL_SITE_SUBSET;

    // create a dummy cudaCloverField if one is not defined
    cf_param.order = QUDA_INVALID_CLOVER_ORDER;
    return std::make_unique<cudaCloverField>(cf_param);
  }

  template <typename Float, int fineColor, int coarseSpin, int coarseColor, typename Arg>
  class CalculateStaggeredY : public TunableVectorYZ {

    Arg &arg;
    const GaugeField &meta;
    GaugeField &Y;
    GaugeField &X;

    long long flops() const { return arg.coarseVolumeCB*coarseSpin*coarseColor; }

    long long bytes() const
    {
      // 2 from forwards / backwards contributions, Y and X are sparse - only needs to write non-zero elements, 2nd term is mass term
      return meta.Bytes() + (2 * meta.Bytes() * Y.Precision()) / meta.Precision() + 2 * 2 * coarseSpin * coarseColor * arg.coarseVolumeCB * X.Precision();
    }

    unsigned int minThreads() const { return arg.fineVolumeCB; }
    bool tuneSharedBytes() const { return false; } // don't tune the grid dimension
    bool tuneGridDim() const { return false; } // don't tune the grid dimension
    bool tuneAuxDim() const { return false; }

  public:
    CalculateStaggeredY(Arg &arg, const GaugeField &meta, GaugeField &Y, GaugeField &X) :
      TunableVectorYZ(fineColor*fineColor, 2),
      arg(arg),
      meta(meta),
      Y(Y),
      X(X)
    {
      if (meta.Location() == QUDA_CUDA_FIELD_LOCATION) {
#ifdef JITIFY
        create_jitify_program("kernels/staggered_coarse_op_kernel.cuh");
#endif
      }
      strcpy(aux, compile_type_str(meta));
      strcpy(aux, meta.AuxString());
      strcat(aux,comm_dim_partitioned_string());
      if (meta.Location() == QUDA_CPU_FIELD_LOCATION) strcat(aux, getOmpThreadStr());
      strcat(aux,",computeStaggeredVUV");
      strcat(aux, (meta.Location()==QUDA_CUDA_FIELD_LOCATION && Y.MemType() == QUDA_MEMORY_MAPPED) ? ",GPU-mapped," :
             meta.Location()==QUDA_CUDA_FIELD_LOCATION ? ",GPU-device," : ",CPU,");
      strcat(aux,"coarse_vol=");
      strcat(aux,X.VolString());
    }

    void apply(const qudaStream_t &stream)
    {
      TuneParam tp = tuneLaunch(*this, getTuning(), QUDA_VERBOSE);

      if (meta.Location() == QUDA_CPU_FIELD_LOCATION) {
        ComputeStaggeredVUVCPU<Float,fineColor,coarseSpin,coarseColor>(arg);
      } else {
#ifdef JITIFY
        using namespace jitify::reflection;
        jitify_error = program->kernel("quda::ComputeStaggeredVUVGPU")
          .instantiate(Type<Float>(),fineColor,coarseSpin,coarseColor,Type<Arg>())
          .configure(tp.grid,tp.block,tp.shared_bytes,stream).launch(arg);
#else // not jitify
        qudaLaunchKernel(ComputeStaggeredVUVGPU<Float,fineColor,coarseSpin,coarseColor,Arg>, tp, stream, arg);
#endif // JITIFY
      }
    }

    bool advanceTuneParam(TuneParam &param) const {
      // only do autotuning if we have device fields
      if (meta.Location() == QUDA_CUDA_FIELD_LOCATION && Y.MemType() == QUDA_MEMORY_DEVICE) return Tunable::advanceTuneParam(param);
      else return false;
    }

    TuneKey tuneKey() const { return TuneKey(meta.VolString(), typeid(*this).name(), aux); }
  };

  /**
     @brief Calculate the coarse-link field, including the coarse clover field.

     @param Y[out] Coarse (fat-)link field accessor
     @param X[out] Coarse clover field accessor
     @param G[in] Fine grid link / gauge field accessor
     @param Y_[out] Coarse link field
     @param X_[out] Coarse clover field
     @param X_[out] Coarse clover inverese field (used as temporary here)
     @param v[in] Packed null-space vectors
     @param G_[in] Fine gauge field
     @param mass[in] Kappa parameter
     @param matpc[in] The type of preconditioning of the source fine-grid operator
   */
  template<typename Float, int fineSpin, int fineColor, int coarseSpin, int coarseColor,
	   typename coarseGauge, typename fineGauge>
  void calculateStaggeredY(coarseGauge &Y, coarseGauge &X, fineGauge &G, GaugeField &Y_, GaugeField &X_,
                           const GaugeField &G_, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    // sanity checks
    if (matpc == QUDA_MATPC_EVEN_EVEN_ASYMMETRIC || matpc == QUDA_MATPC_ODD_ODD_ASYMMETRIC)
      errorQuda("Unsupported coarsening of matpc = %d", matpc);

    // This is the last time we use fineSpin, since this file only coarsens
    // staggered-type ops, not wilson-type AND coarse-type.
    if (fineSpin != 1)
      errorQuda("Input Dirac operator %d should have nSpin=1, not nSpin=%d\n", dirac, fineSpin);
    if (fineColor != 3)
      errorQuda("Input Dirac operator %d should have nColor=3, not nColor=%d\n", dirac, fineColor);

    if (G.Ndim() != 4) errorQuda("Number of dimensions not supported");
    const int nDim = 4;

    int x_size[QUDA_MAX_DIM] = { };
    for (int i=0; i<4; i++) x_size[i] = G_.X()[i];
    x_size[4] = 1;

    int xc_size[QUDA_MAX_DIM] = { };
    for (int i=0; i<4; i++) xc_size[i] = X_.X()[i];
    xc_size[4] = 1;

    int geo_bs[QUDA_MAX_DIM] = { };
    for(int d = 0; d < nDim; d++) geo_bs[d] = x_size[d]/xc_size[d];
    int spin_bs = 0; // 0 -> spin-less types.

    // Calculate VUV in one pass (due to KD-transform) for each dimension,
    // accumulating directly into the coarse gauge field Y

    using Arg = CalculateStaggeredYArg<Float,coarseSpin,fineColor,coarseColor,coarseGauge,fineGauge>;
    Arg arg(Y, X, G, mass, x_size, xc_size, geo_bs, spin_bs);
    CalculateStaggeredY<Float, fineColor, coarseSpin, coarseColor, Arg> y(arg, G_, Y_, X_);

    QudaFieldLocation location = checkLocation(Y_, X_, G_);
    if (getVerbosity() >= QUDA_VERBOSE) printfQuda("Running link coarsening on the %s\n", location == QUDA_CUDA_FIELD_LOCATION ? "GPU" : "CPU");

    // We know exactly what the scale should be: the max of all of the (fat) links.
    double max_scale = G_.abs_max();
    if (getVerbosity() >= QUDA_VERBOSE) printfQuda("Global U_max = %e\n", max_scale);

    if (coarseGauge::fixedPoint()) {
      arg.Y.resetScale(max_scale);
      arg.X.resetScale(max_scale > 2.0*mass ? max_scale : 2.0*mass); // To be safe
      Y_.Scale(max_scale);
      X_.Scale(max_scale > 2.0*mass ? max_scale : 2.0*mass); // To be safe
    }

    // We can technically do a uni-directional build, but becauase
    // the coarse link builds are just permutations plus lots of zeros,
    // it's faster to skip the flip!

    if (getVerbosity() >= QUDA_VERBOSE) printfQuda("Computing VUV\n");
    y.apply(0);

    if (getVerbosity() >= QUDA_VERBOSE) {
      for (int d = 0; d < nDim; d++) printfQuda("Y2[%d] = %e\n", 4+d, Y_.norm2( 4+d ));
      for (int d = 0; d < nDim; d++) printfQuda("Y2[%d] = %e\n", d, Y_.norm2( d ));
    }

    if (getVerbosity() >= QUDA_VERBOSE) printfQuda("X2 = %e\n", X_.norm2(0));
  }

  template <typename Float, typename vFloat, int fineColor, int fineSpin, int coarseColor, int coarseSpin>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g,
                           double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    QudaFieldLocation location = Y.Location();

    if (location == QUDA_CPU_FIELD_LOCATION) {

      constexpr QudaFieldOrder csOrder = QUDA_SPACE_SPIN_COLOR_FIELD_ORDER;
      constexpr QudaGaugeFieldOrder gOrder = QUDA_QDP_GAUGE_ORDER;

      if (T.Vectors(Y.Location()).FieldOrder() != csOrder)
        errorQuda("Unsupported field order %d\n", T.Vectors(Y.Location()).FieldOrder());
      if (g.FieldOrder() != gOrder) errorQuda("Unsupported field order %d\n", g.FieldOrder());

      using gFine = typename gauge::FieldOrder<Float,fineColor,1,gOrder>;
      using gCoarse = typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,vFloat>;

      gFine gAccessor(const_cast<GaugeField&>(g));
      gCoarse yAccessor(const_cast<GaugeField&>(Y));
      gCoarse xAccessor(const_cast<GaugeField&>(X));

      calculateStaggeredY<Float,fineSpin,fineColor,coarseSpin,coarseColor>
        (yAccessor, xAccessor, gAccessor, Y, X, g, mass, dirac, matpc);
    } else {

      constexpr QudaFieldOrder csOrder = QUDA_FLOAT2_FIELD_ORDER;
      constexpr QudaGaugeFieldOrder gOrder = QUDA_FLOAT2_GAUGE_ORDER;

      if (T.Vectors(Y.Location()).FieldOrder() != csOrder)
        errorQuda("Unsupported field order %d\n", T.Vectors(Y.Location()).FieldOrder());
      if (g.FieldOrder() != gOrder) errorQuda("Unsupported field order %d\n", g.FieldOrder());

      using gFine = typename gauge::FieldOrder<Float,fineColor,1,gOrder,true,Float>;
      using gCoarse = typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,vFloat>;

      gFine gAccessor(const_cast<GaugeField&>(g));
      gCoarse yAccessor(const_cast<GaugeField&>(Y));
      gCoarse xAccessor(const_cast<GaugeField&>(X));

      calculateStaggeredY<Float,fineSpin,fineColor,coarseSpin,coarseColor>
        (yAccessor, xAccessor, gAccessor, Y, X, g, mass, dirac, matpc);
    }

  }

  template <typename Float, typename vFloat, int fineColor, int fineSpin, int coarseColor, int coarseSpin, int uvSpin>
  void aggregateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g, const GaugeField &l,
                            const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    // Actually create the temporaries like UV, etc.
    auto location = Y.Location();

    //Create a field UV which holds U*V.  Has the same structure as V,
    ColorSpinorParam UVparam(T.Vectors(location));
    UVparam.create = QUDA_ZERO_FIELD_CREATE;
    UVparam.location = location;
    UVparam.nSpin = uvSpin;
    UVparam.setPrecision(T.Vectors(location).Precision());
    UVparam.mem_type = Y.MemType(); // allocate temporaries to match coarse-grid link field

    ColorSpinorField *uv = ColorSpinorField::Create(UVparam);

    ColorSpinorField *av = (dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) ? ColorSpinorField::Create(UVparam) : &const_cast<ColorSpinorField&>(T.Vectors(location));
    
    GaugeField *Yatomic = &Y;
    GaugeField *Xatomic = &X;

    if (Y.Precision() < QUDA_SINGLE_PRECISION) {
      // we need to coarsen into single precision fields (float or int), so we allocate temporaries for this purpose
      // else we can just coarsen directly into the original fields
      GaugeFieldParam param(X); // use X since we want scalar geometry
      param.location = location;
      param.setPrecision(QUDA_SINGLE_PRECISION, location == QUDA_CUDA_FIELD_LOCATION ? true : false);

      Yatomic = GaugeField::Create(param);
      Xatomic = GaugeField::Create(param);
    }

    // Moving along to the build

    const double kappa = -1.; // cancels a minus sign factor for kappa w/in the dslash application
    const double mu_dummy = 0.; 
    const double mu_factor_dummy = 0.;

    bool need_bidirectional = false;
    if (dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) need_bidirectional = true;

    const int nFace = (dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADPC_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) ? 3 : 1;

    if (Y.Location() == QUDA_CPU_FIELD_LOCATION) {

      constexpr QudaFieldOrder csOrder = QUDA_SPACE_SPIN_COLOR_FIELD_ORDER;
      constexpr QudaGaugeFieldOrder gOrder = QUDA_QDP_GAUGE_ORDER;

      if (T.Vectors(Y.Location()).FieldOrder() != csOrder)
        errorQuda("Unsupported field order %d\n", T.Vectors(Y.Location()).FieldOrder());
      if (g.FieldOrder() != gOrder) errorQuda("Unsupported field order %d\n", g.FieldOrder());

      using V = typename colorspinor::FieldOrderCB<Float,fineSpin,fineColor,coarseColor,csOrder,vFloat>;
      using F = typename colorspinor::FieldOrderCB<Float,uvSpin,fineColor,coarseColor,csOrder,vFloat>;
      using gFine = typename gauge::FieldOrder<Float,fineColor,1,gOrder>;
      using gCoarse = typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,vFloat>;
      using gCoarseAtomic = typename gauge::FieldOrder<Float,coarseColor*coarseSpin,coarseSpin,gOrder,true,storeType>;

      const ColorSpinorField &v = T.Vectors(Y.Location());

      V vAccessor(const_cast<ColorSpinorField&>(v), nFace);
      F uvAccessor(*uv, nFace); // will need 2x the spin components for the KD op
      F avAccessor(*av, nFace);
      gFine gAccessor(const_cast<GaugeField&>(g));
      gFine lAccessor(const_cast<GaugeField&>(g));
      gFine xinvAccessor(const_cast<GaugeField&>(XinvKD));
      gCoarse yAccessor(const_cast<GaugeField&>(Y));
      gCoarse xAccessor(const_cast<GaugeField&>(X));
      gCoarseAtomic yAccessorAtomic(*Yatomic);
      gCoarseAtomic xAccessorAtomic(*Xatomic);

      // the repeated xinvAccessor is intentional
      calculateY<QUDA_CPU_FIELD_LOCATION, false, Float, fineSpin, fineColor, coarseSpin, coarseColor>(
        yAccessor, xAccessor, yAccessorAtomic, xAccessorAtomic, uvAccessor, avAccessor, vAccessor, gAccessor, lAccessor, xinvAccessor, xAccessor, xAccessor,
        Y, X, *Yatomic, *Xatomic, *uv, *av, v, g, l, XinvKD, *dummyClover(), kappa, mass, mu_dummy,
        mu_factor_dummy, dirac, matpc, need_bidirectional, T.fineToCoarse(Y.Location()), T.coarseToFine(Y.Location()));
    } else {

      constexpr QudaFieldOrder csOrder = QUDA_FLOAT2_FIELD_ORDER;
      constexpr QudaGaugeFieldOrder gOrder = QUDA_FLOAT2_GAUGE_ORDER;

      if (T.Vectors(Y.Location()).FieldOrder() != csOrder)
        errorQuda("Unsupported field order %d\n", T.Vectors(Y.Location()).FieldOrder());
      if (g.FieldOrder() != gOrder) errorQuda("Unsupported field order %d\n", g.FieldOrder());

      using V = typename colorspinor::FieldOrderCB<Float, fineSpin, fineColor, coarseColor, csOrder, vFloat, vFloat, false, false>;
      using F = typename colorspinor::FieldOrderCB<Float, uvSpin, fineColor, coarseColor, csOrder, vFloat, vFloat, false, false>; // will need 2x the spin components for the KD op
      using gFine =  typename gauge::FieldOrder<Float,fineColor,1,gOrder,true,Float>;
      using gCoarse = typename gauge::FieldOrder<Float, coarseColor * coarseSpin, coarseSpin, gOrder, true, vFloat>;
      using gCoarseAtomic = typename gauge::FieldOrder<Float, coarseColor * coarseSpin, coarseSpin, gOrder, true, storeType>;

      const ColorSpinorField &v = T.Vectors(Y.Location());

      V vAccessor(const_cast<ColorSpinorField &>(v), nFace);
      F uvAccessor(*uv, nFace); // will need 2x the spin components for the KD op
      F avAccessor(*av, nFace);
      gFine gAccessor(const_cast<GaugeField &>(g));
      gFine lAccessor(const_cast<GaugeField &>(l));
      gFine xinvAccessor(const_cast<GaugeField&>(XinvKD));
      gCoarse yAccessor(const_cast<GaugeField &>(Y));
      gCoarse xAccessor(const_cast<GaugeField &>(X));
      gCoarseAtomic yAccessorAtomic(*Yatomic);
      gCoarseAtomic xAccessorAtomic(*Xatomic);

      // repeated xAccessor are just dummy values
      calculateY<QUDA_CUDA_FIELD_LOCATION, false, Float, fineSpin, fineColor, coarseSpin, coarseColor>(
        yAccessor, xAccessor, yAccessorAtomic, xAccessorAtomic, uvAccessor, avAccessor, vAccessor, gAccessor, lAccessor,
        xinvAccessor, xAccessor, xAccessor, Y, X, *Yatomic, *Xatomic, *uv, *av, v, g, l, XinvKD, *dummyClover(),
        kappa, mass, mu_dummy, mu_factor_dummy, dirac, matpc, need_bidirectional, T.fineToCoarse(Y.Location()),
        T.coarseToFine(Y.Location()));
    }

    // Clean up
    if (Yatomic != &Y) delete Yatomic;
    if (Xatomic != &X) delete Xatomic;

    if (av != nullptr && &T.Vectors(location) != av) delete av;
    if (uv != nullptr) delete uv;

  }

  // template on UV spin, which can be 1 for the non-KD ops but needs to be 2 for the KD op
  template <typename Float, typename vFloat, int fineColor, int fineSpin, int coarseColor, int coarseSpin>
  void aggregateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g, const GaugeField &l,
                           const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    if (dirac == QUDA_STAGGERED_DIRAC || dirac == QUDA_ASQTAD_DIRAC) {
      // uvSpin == 1
      aggregateStaggeredY<Float, vFloat, fineColor, fineSpin, coarseColor, coarseSpin, 1>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
    } else if (dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) {
      // uvSpin == 2
      aggregateStaggeredY<Float, vFloat, fineColor, fineSpin, coarseColor, coarseSpin, 2>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
    } else {
      errorQuda("Unexpected dirac type %d\n", dirac);
    }
  }

  // template on the number of coarse degrees of freedom, branch between naive K-D 
  // and actual aggregation
  template <typename Float, typename vFloat, int fineColor, int fineSpin>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g, const GaugeField &l,
                           const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    const int coarseSpin = 2;
    const int coarseColor = Y.Ncolor() / coarseSpin;

    if (coarseColor == 24) {
      if (T.getTransferType() == QUDA_TRANSFER_COARSE_KD)
        // the permutation routines don't need Yatomic, Xatomic, uv, av
        // need to check this sooner
        calculateStaggeredY<Float,vFloat,fineColor,fineSpin,24,coarseSpin>(Y, X, T, g, mass, dirac, matpc);
      else {
        // free field aggregation
        aggregateStaggeredY<Float,vFloat,fineColor,fineSpin,24,coarseSpin>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
      }
    } else if (coarseColor == 64) {
      aggregateStaggeredY<Float,vFloat,fineColor,fineSpin,64,coarseSpin>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
    } else if (coarseColor == 96) {
      aggregateStaggeredY<Float,vFloat,fineColor,fineSpin,96,coarseSpin>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
    } else { // revisit 3 -> 96 later
      errorQuda("Unsupported number of coarse dof %d\n", Y.Ncolor());
    }
  }

  // template on fine spin
  template <typename Float, typename vFloat, int fineColor>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g, const GaugeField &l,
                           const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    if (T.Vectors().Nspin() == 1) {
      calculateStaggeredY<Float,vFloat,fineColor,1>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
    } else {
      errorQuda("Unsupported number of spins %d\n", T.Vectors(X.Location()).Nspin());
    }
  }

  // template on fine colors
  template <typename Float, typename vFloat>
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g, const GaugeField &l,
                           const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    if (g.Ncolor() == 3) {
      calculateStaggeredY<Float,vFloat,3>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
    } else {
      errorQuda("Unsupported number of colors %d\n", g.Ncolor());
    }
  }

  // template on precision of gauge links, whether or not we're coarsening the KD op, precision of KD field
  void calculateStaggeredY(GaugeField &Y, GaugeField &X, const Transfer &T, const GaugeField &g, const GaugeField &l,
                           const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
#if defined(GPU_MULTIGRID) && defined(GPU_STAGGERED_DIRAC)
    checkPrecision(T.Vectors(X.Location()), X, Y);

    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("Computing Y field......\n");

    if (Y.Precision() == QUDA_DOUBLE_PRECISION) {
#ifdef GPU_MULTIGRID_DOUBLE
      if (T.Vectors(X.Location()).Precision() == QUDA_DOUBLE_PRECISION) {
        calculateStaggeredY<double,double>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
      } else {
        errorQuda("Unsupported precision %d\n", Y.Precision());
      }
#else
      errorQuda("Double precision multigrid has not been enabled");
#endif
    } else 
#if QUDA_PRECISION & 4
    if (Y.Precision() == QUDA_SINGLE_PRECISION) {
      if (T.Vectors(X.Location()).Precision() == QUDA_SINGLE_PRECISION) {
        calculateStaggeredY<float,float>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
      } else {
        errorQuda("Unsupported precision %d\n", T.Vectors(X.Location()).Precision());
      }
    } else 
#endif
#if QUDA_PRECISION & 2
    if (Y.Precision() == QUDA_HALF_PRECISION) {
      if (T.Vectors(X.Location()).Precision() == QUDA_HALF_PRECISION) {
        calculateStaggeredY<float,short>(Y, X, T, g, l, XinvKD, mass, dirac, matpc);
      } else {
        errorQuda("Unsupported precision %d\n", T.Vectors(X.Location()).Precision());
      }
    } else
#endif
    {
      errorQuda("Unsupported precision %d\n", Y.Precision());
    }
    if (getVerbosity() >= QUDA_SUMMARIZE) printfQuda("....done computing Y field\n");
#else
    errorQuda("Staggered multigrid has not been built");
#endif
  }

  //Calculates the coarse color matrix and puts the result in Y.
  //N.B. Assumes Y, X have been allocated.
  void StaggeredCoarseOp(GaugeField &Y, GaugeField &X, const Transfer &T, const cudaGaugeField &gauge,
                         const cudaGaugeField &longGauge, const GaugeField &XinvKD, double mass, QudaDiracType dirac, QudaMatPCType matpc)
  {
    QudaPrecision precision = Y.Precision();
    QudaFieldLocation location = checkLocation(Y, X);

    // sanity check long link coarsening
    if ((dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) && &gauge == &longGauge)
      errorQuda("Dirac type is %d but fat and long gauge links alias", dirac);

    // sanity check KD op coarsening
    if ((dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) && &gauge == &XinvKD)
      errorQuda("Dirac type is %d but fat links and KD inverse fields alias", dirac);

    if (dirac == QUDA_ASQTADKD_DIRAC && &longGauge == &XinvKD)
      errorQuda("Dirac type is %d but long links and KD inverse fields alias", dirac);

    if ((dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) && XinvKD.Reconstruct() != QUDA_RECONSTRUCT_NO)
      errorQuda("Invalid reconstruct %d for KD inverse field", XinvKD.Reconstruct());

    GaugeField *U = location == QUDA_CUDA_FIELD_LOCATION ? const_cast<cudaGaugeField*>(&gauge) : nullptr;
    GaugeField *L = (location == QUDA_CUDA_FIELD_LOCATION && (dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC)) ? const_cast<cudaGaugeField*>(&longGauge) : nullptr;
    GaugeField *Xinv = (location == QUDA_CUDA_FIELD_LOCATION && (dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC)) ? const_cast<GaugeField*>(&XinvKD) : nullptr;

    if (location == QUDA_CPU_FIELD_LOCATION) {
      //First make a cpu gauge field from the cuda gauge field
      int pad = 0;
      GaugeFieldParam gf_param(gauge.X(), precision, QUDA_RECONSTRUCT_NO, pad, gauge.Geometry());
      gf_param.order = QUDA_QDP_GAUGE_ORDER;
      gf_param.fixed = gauge.GaugeFixed();
      gf_param.link_type = gauge.LinkType();
      gf_param.t_boundary = gauge.TBoundary();
      gf_param.anisotropy = gauge.Anisotropy();
      gf_param.gauge = nullptr;
      gf_param.create = QUDA_NULL_FIELD_CREATE;
      gf_param.siteSubset = QUDA_FULL_SITE_SUBSET;
      gf_param.nFace = 1;
      gf_param.ghostExchange = QUDA_GHOST_EXCHANGE_PAD;

      U = new cpuGaugeField(gf_param);

      //Copy the cuda gauge field to the cpu
      gauge.saveCPUField(*static_cast<cpuGaugeField*>(U));

      // Create either a real or a dummy L field
      GaugeFieldParam lgf_param(longGauge.X(), precision, QUDA_RECONSTRUCT_NO, pad, longGauge.Geometry());
      if (!(dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC))
        for (int i = 0; i < lgf_param.nDim; i++) lgf_param.x[i] = 0;
      lgf_param.order = QUDA_QDP_GAUGE_ORDER;
      lgf_param.fixed = longGauge.GaugeFixed();
      lgf_param.link_type = longGauge.LinkType();
      lgf_param.t_boundary = longGauge.TBoundary();
      lgf_param.anisotropy = longGauge.Anisotropy();
      lgf_param.gauge = nullptr;
      lgf_param.create = QUDA_NULL_FIELD_CREATE;
      lgf_param.siteSubset = QUDA_FULL_SITE_SUBSET;
      lgf_param.nFace = 3;
      lgf_param.ghostExchange = QUDA_GHOST_EXCHANGE_PAD;

      L = new cpuGaugeField(lgf_param);

      //Copy the cuda gauge field to the cpu
      if (dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC)
        longGauge.saveCPUField(*static_cast<cpuGaugeField*>(L));
      
      // Create either a real or a dummy Xinv field
      GaugeFieldParam xgf_param(XinvKD.X(), precision, QUDA_RECONSTRUCT_NO, pad, XinvKD.Geometry());
      if (!(dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC))
        for (int i = 0; i < xgf_param.nDim; i++) xgf_param.x[i] = 0;
      xgf_param.order = QUDA_QDP_GAUGE_ORDER;
      xgf_param.fixed = XinvKD.GaugeFixed();
      xgf_param.link_type = XinvKD.LinkType();
      xgf_param.t_boundary = XinvKD.TBoundary();
      xgf_param.anisotropy = XinvKD.Anisotropy();
      if (dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) {
        xgf_param.create = QUDA_COPY_FIELD_CREATE;
      } else {
        xgf_param.gauge = nullptr;
        xgf_param.create = QUDA_NULL_FIELD_CREATE;
      }
      xgf_param.siteSubset = QUDA_FULL_SITE_SUBSET;
      xgf_param.nFace = 0;
      xgf_param.ghostExchange = QUDA_GHOST_EXCHANGE_NO;

      Xinv = new cpuGaugeField(xgf_param);

      //Copy the cuda gauge field to the cpu
      //if (dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC)
      //  XinvKD.saveCPUField(*static_cast<cpuGaugeField*>(Xinv));

      
    } else if (location == QUDA_CUDA_FIELD_LOCATION) {

      int pad = 0;

      // create some dummy fields if need be
      if (!(dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC)) {
        // create a dummy field
        GaugeFieldParam lgf_param(longGauge);
        for (int i = 0; i < lgf_param.nDim; i++) lgf_param.x[i] = 0;
        lgf_param.reconstruct = QUDA_RECONSTRUCT_NO;
        lgf_param.order = QUDA_FLOAT2_GAUGE_ORDER;
        lgf_param.setPrecision(lgf_param.Precision());
        lgf_param.create = QUDA_NULL_FIELD_CREATE;
        L = new cudaGaugeField(lgf_param);
      } else if ((dirac == QUDA_ASQTAD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC) && longGauge.Reconstruct() != QUDA_RECONSTRUCT_NO) {
        // create a copy of the gauge field with no reconstruction
        GaugeFieldParam lgf_param(longGauge);
        lgf_param.reconstruct = QUDA_RECONSTRUCT_NO;
        lgf_param.order = QUDA_FLOAT2_GAUGE_ORDER;
        lgf_param.setPrecision(lgf_param.Precision());
        L = new cudaGaugeField(lgf_param);

        L->copy(longGauge);
      }

      if (!(dirac == QUDA_STAGGEREDKD_DIRAC || dirac == QUDA_ASQTADKD_DIRAC)) {
        // Create a dummy field
        GaugeFieldParam xgf_param(XinvKD.X(), precision, QUDA_RECONSTRUCT_NO, pad, XinvKD.Geometry());
        for (int i = 0; i < xgf_param.nDim; i++) xgf_param.x[i] = 0;
        xgf_param.reconstruct = QUDA_RECONSTRUCT_NO;
        xgf_param.order = QUDA_FLOAT2_GAUGE_ORDER;
        xgf_param.setPrecision(xgf_param.Precision());
        xgf_param.create = QUDA_NULL_FIELD_CREATE;
        Xinv = new cudaGaugeField(xgf_param);
      }
      // no need to worry about XinvKD's reconstruct

      if (gauge.Reconstruct() != QUDA_RECONSTRUCT_NO) {
        //Create a copy of the gauge field with no reconstruction, required for fine-grained access
        GaugeFieldParam gf_param(gauge);
        gf_param.reconstruct = QUDA_RECONSTRUCT_NO;
        gf_param.order = QUDA_FLOAT2_GAUGE_ORDER;
        gf_param.setPrecision(gf_param.Precision());
        U = new cudaGaugeField(gf_param);

        U->copy(gauge);
      }
      
    }

    calculateStaggeredY(Y, X, T, *U, *L, *Xinv, mass, dirac, matpc);

    if (U != &gauge) delete U;
    if (L != &longGauge) delete L;
    if (Xinv != &XinvKD) delete Xinv;
  }

} //namespace quda
