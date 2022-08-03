#pragma once

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include <quda.h>
#include <gauge_field.h>
#include <dirac_quda.h>
#include <misc.h>
#include <host_utils.h>
#include <command_line_params.h>

#include <gsmear_test_helpers.h>
#include <assert.h>
#include <gtest/gtest.h>

using namespace quda;

void initExtendedField(void* sitelink_ex[4], void* sitelink[4]) 
{	
  int X1=Z[0];
  int X2=Z[1];
  int X3=Z[2];
  int X4=Z[3];

  for(int i=0; i < V_ex; i++){
    int sid = i;
    int oddBit=0;
    if(i >= Vh_ex){
      sid = i - Vh_ex;
      oddBit = 1;
    }

    int za = sid/E1h;
    int x1h = sid - za*E1h;
    int zb = za/E2;
    int x2 = za - zb*E2;
    int x4 = zb/E3;
    int x3 = zb - x4*E3;
    int x1odd = (x2 + x3 + x4 + oddBit) & 1;
    int x1 = 2*x1h + x1odd;


    if( x1< 2 || x1 >= X1 +2
        || x2< 2 || x2 >= X2 +2
        || x3< 2 || x3 >= X3 +2
        || x4< 2 || x4 >= X4 +2){

      continue;

    }

    x1 = (x1 - 2 + X1) % X1;
    x2 = (x2 - 2 + X2) % X2;
    x3 = (x3 - 2 + X3) % X3;
    x4 = (x4 - 2 + X4) % X4;

    int idx = (x4*X3*X2*X1+x3*X2*X1+x2*X1+x1)>>1;
    if(oddBit){
      idx += Vh;
    }
    for(int dir= 0; dir < 4; dir++){
      char* src = (char*)sitelink[dir];
      char* dst = (char*)sitelink_ex[dir];
      memcpy(dst + i * gauge_site_size * host_gauge_data_type_size,
             src + idx * gauge_site_size * host_gauge_data_type_size, gauge_site_size * host_gauge_data_type_size);
    }//dir
  }//i
  return;
}


gsmear_test_type gtest_type = gsmear_test_type::TwoLink;

CLI::TransformPairs<gsmear_test_type> gtest_type_map {
  {"TwoLink", gsmear_test_type::TwoLink}, {"GaussianSmear", gsmear_test_type::GaussianSmear}
};

struct GSmearTime {//DslashTime
  double event_time;
  double cpu_time;
  double cpu_min;
  double cpu_max;

  GSmearTime() : event_time(0.0), cpu_time(0.0), cpu_min(DBL_MAX), cpu_max(0.0) { }
};

struct StaggeredGSmearTestWrapper {//

  bool is_ctest = false; 

  // Allocate host staggered gauge fields
  void* qdp_inlink[4]    = {nullptr,nullptr,nullptr,nullptr};
  void* qdp_twolnk[4]    = {nullptr,nullptr,nullptr,nullptr};  
  void* qdp_inlink_ex[4] = {nullptr,nullptr,nullptr,nullptr};//sitelink_ex
  
  void *milc_inlink       = nullptr;
  void *milc_twolnk       = nullptr;
  //
  GaugeField *cpuTwoLink  = nullptr;

  QudaGaugeParam  gauge_param;//
  QudaInvertParam inv_param;
  //
  std::unique_ptr<ColorSpinorField> spinor;
  std::unique_ptr<ColorSpinorField> spinorRef;
  std::unique_ptr<ColorSpinorField> tmp;
  std::unique_ptr<ColorSpinorField> tmp2;
  // For loading the gauge fields
  int argc_copy;
  char **argv_copy;

  void staggeredGSmearRef()
  {
    // compare to dslash reference implementation
    printfQuda("Calculating reference implementation...");
    switch (gtest_type) {
      case gsmear_test_type::TwoLink:
	      {
        computeTwoLinkCPU(qdp_twolnk, qdp_inlink_ex, &gauge_param);
        break;
	      }
      case gsmear_test_type::GaussianSmear:
	{
        const double ftmp    = -(smear_coeff*smear_coeff)/(4.0*n_steps*4.0);
        const double msq     = 1. / ftmp;
        const double a       = inv_param.laplace3D * 2.0 + msq;

        for (int i = 0; i < n_steps; i++) {
          if (i > 0) std::swap(*tmp, *spinorRef);
      
          quda::blas::ax(ftmp, *tmp);
          quda::blas::axpy(a, *tmp, *tmp2);
      
          staggeredTwoLinkGaussianSmear(spinorRef->Even(), qdp_twolnk, (void **)cpuTwoLink->Ghost(),  tmp->Even(), &gauge_param, &inv_param, 0, smear_coeff, t0, gauge_param.cpu_prec);
          staggeredTwoLinkGaussianSmear(spinorRef->Odd(),  qdp_twolnk, (void **)cpuTwoLink->Ghost(),  tmp->Odd(),  &gauge_param, &inv_param, 1, smear_coeff, t0, gauge_param.cpu_prec);
          //blas::xpay(*tmp2, -1.0, *spinorRef);
          xpay(tmp2->Even().V(), -1.0, spinorRef->Even().V(), spinor->Even().Length(), gauge_param.cpu_prec);
          xpay(tmp2->Odd().V(),  -1.0, spinorRef->Odd().V(), spinor->Odd().Length(), gauge_param.cpu_prec);
          //
          memset(tmp2->Even().V(), 0, spinor->Even().Length() * gauge_param.cpu_prec);
          memset(tmp2->Odd().V(), 0, spinor->Odd().Length() * gauge_param.cpu_prec);
        }
        break;
	}
      default: errorQuda("Test type not defined");
    }
  }

  void init_ctest_once()
  {
    static bool has_been_called = false;
    if (has_been_called) { errorQuda("This function is not supposed to be called twice.\n"); }
    is_ctest = true; // Is being used in dslash_ctest.
    has_been_called = true;
  }

  void end_ctest_once()
  {
    static bool has_been_called = false;
    if (has_been_called) { errorQuda("This function is not supposed to be called twice.\n"); }
    has_been_called = true;
  }

  void init_ctest(int precision, QudaReconstructType link_recon_)
  {
    gauge_param = newQudaGaugeParam();
    inv_param   = newQudaInvertParam();

    setStaggeredGaugeParam(gauge_param);
    setStaggeredInvertParam(inv_param);

    auto prec = getPrecision(precision);
    setVerbosity(QUDA_SUMMARIZE);

    gauge_param.cuda_prec = prec;
    gauge_param.cuda_prec_sloppy = prec;
    gauge_param.cuda_prec_precondition = prec;
    gauge_param.cuda_prec_refinement_sloppy = prec;

    inv_param.cuda_prec = prec;

    link_recon = link_recon_;

    init();
  }

  void init_test()
  {
    gauge_param = newQudaGaugeParam();
    inv_param   = newQudaInvertParam();

    setStaggeredGaugeParam(gauge_param);
    setStaggeredInvertParam(inv_param);

    init();
  }

  void init()
  {
    inv_param.split_grid[0] = grid_partition[0];
    inv_param.split_grid[1] = grid_partition[1];
    inv_param.split_grid[2] = grid_partition[2];
    inv_param.split_grid[3] = grid_partition[3];

    //num_src = grid_partition[0] * grid_partition[1] * grid_partition[2] * grid_partition[3];

    setDims(gauge_param.X);
    dw_setDims(gauge_param.X, 1);
    // Allocate a lot of memory because I'm very confused
    for (int dir = 0; dir < 4; dir++) {
      qdp_inlink[dir]    = safe_malloc(V    * gauge_site_size * host_gauge_data_type_size);
      qdp_twolnk[dir]    = safe_malloc(V    * gauge_site_size * host_gauge_data_type_size);
      qdp_inlink_ex[dir] = safe_malloc(V_ex * gauge_site_size * host_gauge_data_type_size);    
    }
    //
    milc_inlink = safe_malloc(4 * V * gauge_site_size * host_gauge_data_type_size);
    milc_twolnk = safe_malloc(4 * V * gauge_site_size * host_gauge_data_type_size);

    gauge_param.reconstruct = QUDA_RECONSTRUCT_NO;

    constructHostGaugeField(qdp_inlink, gauge_param, argc_copy, argv_copy);
    initExtendedField(qdp_inlink_ex, qdp_inlink);
    // Prepare two link field:
    if(verify_results or (gtest_type == gsmear_test_type::GaussianSmear and  compute_two_link == false)) computeTwoLinkCPU(qdp_twolnk, qdp_inlink_ex, &gauge_param);

    // Reorder gauge fields to MILC order
    if(compute_two_link or gtest_type == gsmear_test_type::TwoLink)
      reorderQDPtoMILC(milc_inlink, qdp_inlink, V, gauge_site_size, gauge_param.cpu_prec, gauge_param.cpu_prec);
    else
      reorderQDPtoMILC(milc_inlink, qdp_twolnk, V, gauge_site_size, gauge_param.cpu_prec, gauge_param.cpu_prec);

    if ( gtest_type == gsmear_test_type::GaussianSmear ) {
      // Specific gauge parameters for MILC
      int link_pad = 3 * gauge_param.ga_pad;

      gauge_param.reconstruct                   = QUDA_RECONSTRUCT_NO;
      gauge_param.reconstruct_sloppy            = QUDA_RECONSTRUCT_NO;
      gauge_param.reconstruct_refinement_sloppy = QUDA_RECONSTRUCT_NO;

      gauge_param.type                 = QUDA_ASQTAD_LONG_LINKS;
      gauge_param.ga_pad               = link_pad;
      gauge_param.staggered_phase_type = QUDA_STAGGERED_PHASE_NO;
    }
    //
    loadGaugeQuda(milc_inlink, &gauge_param);
    //
    // Create ghost gauge fields in case of multi GPU builds.
    reorderQDPtoMILC(milc_twolnk, qdp_twolnk, V, gauge_site_size, gauge_param.cpu_prec, gauge_param.cpu_prec);
  
    gauge_param.type     = QUDA_ASQTAD_LONG_LINKS;
    gauge_param.location = QUDA_CPU_FIELD_LOCATION;

    GaugeFieldParam cpuTwoLinkParam(gauge_param, milc_twolnk);
    cpuTwoLinkParam.ghostExchange = QUDA_GHOST_EXCHANGE_PAD;
    cpuTwoLink                    = GaugeField::Create(cpuTwoLinkParam);    
    
    if(gtest_type == gsmear_test_type::GaussianSmear) {
      ColorSpinorParam cs_param;

      constructStaggeredTestSpinorParam(&cs_param, &inv_param, &gauge_param);

      spinor    = std::make_unique<ColorSpinorField>(cs_param);
      spinorRef = std::make_unique<ColorSpinorField>(cs_param);
      tmp       = std::make_unique<ColorSpinorField>(cs_param);
      tmp2      = std::make_unique<ColorSpinorField>(cs_param);    

      spinor->Source(QUDA_RANDOM_SOURCE);
    
      *tmp = *spinor;
    }
  }

  void end()
  {
    // Clean up gauge fields
    for (int dir = 0; dir < 4; dir++) {
      host_free(qdp_inlink[dir]);
      host_free(qdp_inlink_ex[dir]);
      host_free(qdp_twolnk[dir]);
    }
  
    host_free(milc_inlink);
    host_free(milc_twolnk);

    if (cpuTwoLink != nullptr) { delete cpuTwoLink; cpuTwoLink = nullptr; }

    if(gtest_type == gsmear_test_type::GaussianSmear) {
      tmp2.reset();
      tmp.reset();
      spinor.reset();
      spinorRef.reset();
    }

    freeGaugeQuda();

    commDimPartitionedReset();
  }

  GSmearTime gsmearCUDA(int niter)
  {
    GSmearTime gsmear_time;

    host_timer_t host_timer;
    device_timer_t device_timer;

    comm_barrier();
    device_timer.start();

    // smearing parameters
    switch (gtest_type) {
      case gsmear_test_type::TwoLink:
	      {
        computeTwoLinkQuda((void*) qdp_twolnk, nullptr, &gauge_param);
	break;
	      }
      case gsmear_test_type::GaussianSmear:
	      {
        double omega       = 2.0;
        int n_steps        = 50;
        double smear_coeff = -1.0 * omega * omega / ( 4*n_steps );

        const int compute_2link = 1;
        const int delete_2link  = 0;
        const int t0            = -1;
  
        performTwoLinkGaussianSmearNStep(spinor->V(), &inv_param, n_steps, smear_coeff, compute_2link, delete_2link, t0);

        break;
	      }
        default: errorQuda("Test type not defined");
    }

    device_timer.stop();
    gsmear_time.event_time = device_timer.last();

    return gsmear_time;
  }

  void run_test(int niter, bool print_metrics = false)
  {
    printfQuda("Tuning...\n");
    gsmearCUDA(1);


    GSmearTime gsmear_time = gsmearCUDA(niter);
    if(gtest_type == gsmear_test_type::GaussianSmear) *spinorRef = *spinor;

    if (print_metrics) {
      printfQuda("%fus per kernel call\n", 1e6 * gsmear_time.event_time / niter);

      unsigned long long flops = 0.0;//FIXME dirac->Flops();
      double gflops = 1.0e-9 * flops / gsmear_time.event_time;
      printfQuda("GFLOPS = %f\n", gflops);
      ::testing::Test::RecordProperty("Gflops", std::to_string(gflops));

      size_t ghost_bytes = spinor->GhostBytes();

      ::testing::Test::RecordProperty("Halo_bidirectitonal_BW_GPU",
                                      1.0e-9 * 2 * ghost_bytes * niter / gsmear_time.event_time);
      ::testing::Test::RecordProperty("Halo_bidirectitonal_BW_CPU",
                                      1.0e-9 * 2 * ghost_bytes * niter / gsmear_time.cpu_time);
      ::testing::Test::RecordProperty("Halo_bidirectitonal_BW_CPU_min", 1.0e-9 * 2 * ghost_bytes / gsmear_time.cpu_max);
      ::testing::Test::RecordProperty("Halo_bidirectitonal_BW_CPU_max", 1.0e-9 * 2 * ghost_bytes / gsmear_time.cpu_min);
      ::testing::Test::RecordProperty("Halo_message_size_bytes", 2 * ghost_bytes);

      printfQuda(
        "Effective halo bi-directional bandwidth (GB/s) GPU = %f ( CPU = %f, min = %f , max = %f ) for aggregate "
        "message size %lu bytes\n",
        1.0e-9 * 2 * ghost_bytes * niter / gsmear_time.event_time,
        1.0e-9 * 2 * ghost_bytes * niter / gsmear_time.cpu_time, 1.0e-9 * 2 * ghost_bytes / gsmear_time.cpu_max,
        1.0e-9 * 2 * ghost_bytes / gsmear_time.cpu_min, 2 * ghost_bytes);
    }
  }

  double verify()
  {
    double deviation = 0.0;

    //double spinor_ref_norm = blas::norm2(*spinorRef);
    //double spinor_out_norm = blas::norm2(*spinorOut);

    //bool failed = false;
    // Catching nans is weird.
    //if (std::isnan(spinor_ref_norm)) { failed = true; }
    //if (std::isnan(spinor_out_norm)) { failed = true; }

    //printfQuda("Results: CPU = %f, QUDA = %f, L2 relative deviation = %e\n",
    //             spinor_ref_norm, spinor_out_norm, 1.0 - sqrt(spinor_out_norm / spinor_ref_norm));
    //deviation = pow(10, -(double)(ColorSpinorField::Compare(*spinorRef, *spinorOut)));
    //if (failed) { deviation = 1.0; }

    return deviation;
  }
};
