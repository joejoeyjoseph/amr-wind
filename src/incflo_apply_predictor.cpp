#include <incflo.H>

using namespace amrex;
//
// Apply predictor:
//
//  1. Use u = vel_old to compute
//
//      conv_u  = - u grad u
//      conv_r  = - div( u rho  )
//      conv_t  = - div( u trac )
//      eta_old     = visosity at m_cur_time
//      if (m_diff_type == DiffusionType::Explicit)
//         divtau _old = div( eta ( (grad u) + (grad u)^T ) ) / rho
//         rhs = u + dt * ( conv + divtau_old )
//      else
//         divtau_old  = 0.0
//         rhs = u + dt * conv
//
//      eta     = eta at new_time
//
//  2. Add explicit forcing term i.e. gravity + lagged pressure gradient
//
//      rhs += dt * ( g - grad(p + p0) / rho )
//
//      Note that in order to add the pressure gradient terms divided by rho,
//      we convert the velocity to momentum before adding and then convert them back.
//
//  3. A. If (m_diff_type == DiffusionType::Implicit)
//        solve implicit diffusion equation for u*
//
//     ( 1 - dt / rho * div ( eta grad ) ) u* = u^n + dt * conv_u
//                                                  + dt * ( g - grad(p + p0) / rho )
//
//     B. If (m_diff_type == DiffusionType::Crank-Nicolson)
//        solve semi-implicit diffusion equation for u*
//
//     ( 1 - (dt/2) / rho * div ( eta_old grad ) ) u* = u^n + dt * conv_u + (dt/2) / rho * div (eta_old grad) u^n
//                                                          + dt * ( g - grad(p + p0) / rho )
//
//  4. Apply projection
//
//     Add pressure gradient term back to u*:
//
//      u** = u* + dt * grad p / rho
//
//     Solve Poisson equation for phi:
//
//     div( grad(phi) / rho ) = div( u** )
//
//     Update pressure:
//
//     p = phi / dt
//
//     Update velocity, now divergence free
//
//     vel = u** - dt * grad p / rho
//
// It is assumed that the ghost cels of the old data have been filled and
// the old and new data are the same in valid region.
//
void incflo::ApplyPredictor (bool incremental_projection)
{
    BL_PROFILE("incflo::ApplyPredictor");

    // We use the new time value for things computed on the "*" state
    Real new_time = m_cur_time + m_dt;

    if (m_verbose > 2)
    {
        amrex::Print() << "Before predictor step:" << std::endl;
        PrintMaxValues(new_time);
    }

    Vector<MultiFab> vel_forces, tra_forces;
    Vector<MultiFab> vel_eta, tra_eta;
    for (int lev = 0; lev <= finest_level; ++lev) {
        vel_forces.emplace_back(grids[lev], dmap[lev], AMREX_SPACEDIM, nghost_force(),
                                MFInfo(), Factory(lev));

        if (m_advect_tracer) {
            tra_forces.emplace_back(grids[lev], dmap[lev], m_ntrac, nghost_force(),
                                    MFInfo(), Factory(lev));
        }
        vel_eta.emplace_back(grids[lev], dmap[lev], 1, 1, MFInfo(), Factory(lev));
        if (m_advect_tracer) {
            tra_eta.emplace_back(grids[lev], dmap[lev], m_ntrac, 1, MFInfo(), Factory(lev));
        }
    }

    compute_vel_forces(GetVecOfPtrs(vel_forces), get_velocity_old_const(), 
                                                 get_density_old_const(), get_tracer_old_const());
    compute_tra_forces(GetVecOfPtrs(tra_forces));

    compute_viscosity(GetVecOfPtrs(vel_eta), GetVecOfPtrs(tra_eta),
                      get_density_old_const(), get_velocity_old_const(), get_tracer_old_const(),
                      m_cur_time, 1);

    if (need_divtau()) {
        get_diffusion_tensor_op()->compute_divtau(get_divtau_old(),
                                                  get_velocity_old_const(),
                                                  get_density_old_const(),
                                                  GetVecOfConstPtrs(vel_eta));
        for (int lev = 0; lev <= finest_level; ++lev) {
            MultiFab::Add(vel_forces[lev], m_leveldata[lev]->divtau_o, 0, 0, AMREX_SPACEDIM, 0);
        }

        if (m_advect_tracer) {
            get_diffusion_scalar_op()->compute_laps(get_laps_old(),
                                                    get_tracer_old_const(),
                                                    get_density_old_const(),
                                                    GetVecOfConstPtrs(tra_eta));
            for (int lev = 0; lev <= finest_level; ++lev) {
                MultiFab::Add(tra_forces[lev], m_leveldata[lev]->laps_o, 0, 0, m_ntrac, 0);
            }
        }
    }

    if (m_use_godunov and nghost_force() > 0) {
        fillpatch_force(m_cur_time, GetVecOfPtrs(vel_forces), nghost_force());
        if (m_advect_tracer) {
            fillpatch_force(m_cur_time, GetVecOfPtrs(tra_forces), nghost_force());
        }
    }

    Vector<MultiFab> u_mac(finest_level+1), v_mac(finest_level+1), w_mac(finest_level+1);
    int ngmac = nghost_mac();

    for (int lev = 0; lev <= finest_level; ++lev) {
        u_mac[lev].define(amrex::convert(grids[lev],IntVect::TheDimensionVector(0)), dmap[lev],
                          1, ngmac, MFInfo(), Factory(lev));
        v_mac[lev].define(amrex::convert(grids[lev],IntVect::TheDimensionVector(1)), dmap[lev],
                          1, ngmac, MFInfo(), Factory(lev));
        w_mac[lev].define(amrex::convert(grids[lev],IntVect::TheDimensionVector(2)), dmap[lev],
                          1, ngmac, MFInfo(), Factory(lev));
        if (ngmac > 0) {
            u_mac[lev].setBndry(0.0);
            v_mac[lev].setBndry(0.0);
            w_mac[lev].setBndry(0.0);
        }
    }

    // if ( m_use_godunov) Compute the explicit advective terms R_u^(n+1/2), R_s^(n+1/2) and R_t^(n+1/2)
    // if (!m_use_godunov) Compute the explicit advective terms R_u^n      , R_s^n       and R_t^n
    compute_convective_term(get_conv_velocity_old(), get_conv_density_old(), get_conv_tracer_old(),
                            get_velocity_old_const(), get_density_old_const(), get_tracer_old_const(),
                            GetVecOfPtrs(u_mac), GetVecOfPtrs(v_mac),
                            GetVecOfPtrs(w_mac), 
                            GetVecOfConstPtrs(vel_forces), GetVecOfConstPtrs(tra_forces),
                            m_cur_time);

    // Define local variables for lambda to capture.
    Real l_dt = m_dt;
    bool l_constant_density = m_constant_density;
    int l_ntrac = (m_advect_tracer) ? m_ntrac : 0;
    for (int lev = 0; lev <= finest_level; lev++)
    {
        auto& ld = *m_leveldata[lev];
#ifdef _OPENMP
#pragma omp parallel if (Gpu::notInLaunchRegion())
#endif
        for (MFIter mfi(ld.velocity,TilingIfNotGPU()); mfi.isValid(); ++mfi)
        {
            Box const& bx = mfi.tilebox();
            Array4<Real> const& vel = ld.velocity.array(mfi);
            Array4<Real> const& rho = ld.density.array(mfi);
            Array4<Real> const& tra = ld.tracer.array(mfi);
            Array4<Real const> const& dvdt = ld.conv_velocity_o.const_array(mfi);
            Array4<Real const> const& drdt = ld.conv_density_o.const_array(mfi);
            Array4<Real const> const& dtdt = ld.conv_tracer_o.const_array(mfi);
            Array4<Real const> const& vel_f = vel_forces[lev].const_array(mfi);
            Array4<Real const> const& tra_f = (l_ntrac > 0) ? tra_forces[lev].const_array(mfi)
                                                            : Array4<Real const>{};
            // if need_divtau()==true, the forces have already included diffusion terms
            if (need_divtau() and m_diff_type != DiffusionType::Explicit) {
                Array4<Real const> const& divtau = ld.divtau_o.const_array(mfi);
                Array4<Real const> laps;
                if (m_advect_tracer) {
                    laps = ld.laps_o.const_array(mfi);
                }
                // It's either implicit or Crank-Nicolson.
                Real s = (m_diff_type == DiffusionType::Implicit) ? -1.0 : -0.5;
                amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
                {
                    vel(i,j,k,0) += l_dt*(dvdt(i,j,k,0)+vel_f(i,j,k,0)+s*divtau(i,j,k,0));
                    vel(i,j,k,1) += l_dt*(dvdt(i,j,k,1)+vel_f(i,j,k,1)+s*divtau(i,j,k,1));
                    vel(i,j,k,2) += l_dt*(dvdt(i,j,k,2)+vel_f(i,j,k,2)+s*divtau(i,j,k,2));

                    if (!l_constant_density) {
                        rho(i,j,k) += l_dt * drdt(i,j,k);
                    }

                    for (int n = 0; n < l_ntrac; ++n) {
                        tra(i,j,k,n) += l_dt * (dtdt(i,j,k,n)+tra_f(i,j,k,n)+s*laps(i,j,k,n));
                    }
                });
            } else {
                amrex::ParallelFor(bx, [=] AMREX_GPU_DEVICE (int i, int j, int k) noexcept
                {
                    vel(i,j,k,0) += l_dt*(dvdt(i,j,k,0)+vel_f(i,j,k,0));
                    vel(i,j,k,1) += l_dt*(dvdt(i,j,k,1)+vel_f(i,j,k,1));
                    vel(i,j,k,2) += l_dt*(dvdt(i,j,k,2)+vel_f(i,j,k,2));

                    if (!l_constant_density) {
                        rho(i,j,k) += l_dt * drdt(i,j,k);
                    }

                    for (int n = 0; n < l_ntrac; ++n) {
                        tra(i,j,k,n) += l_dt * (dtdt(i,j,k,n)+tra_f(i,j,k,n));
                    }
                });
            }
        }
    }

    // Solve diffusion equation for u* but using eta_old at old time
    if (m_diff_type == DiffusionType::Crank_Nicolson || m_diff_type == DiffusionType::Implicit)
    {
        const int ng_diffusion = 1;
        for (int lev = 0; lev <= finest_level; ++lev) {
            fillphysbc_velocity(lev, new_time, m_leveldata[lev]->velocity, ng_diffusion);
            if (m_advect_tracer) {
                fillphysbc_tracer(lev, new_time, m_leveldata[lev]->tracer, ng_diffusion);
            }
        }

        Real dt_diff = (m_diff_type == DiffusionType::Implicit) ? m_dt : 0.5*m_dt;
        get_diffusion_tensor_op()->diffuse_velocity(get_velocity_new(),
                                                    get_density_new(),
                                                    GetVecOfConstPtrs(vel_eta),
                                                    dt_diff);
        if (m_advect_tracer) {
            get_diffusion_scalar_op()->diffuse_scalar(get_tracer_new(),
                                                      get_density_new(),
                                                      GetVecOfConstPtrs(tra_eta),
                                                      dt_diff);
        }
    }

    // **********************************************************************************************
    // 
    // Project velocity field, update pressure
    // 
    // **********************************************************************************************
    ApplyProjection(new_time, m_dt, incremental_projection);

    // **********************************************************************************************
    // 
    // Over-write velocity in cells with vfrac < 1e-4
    // 
    // **********************************************************************************************
    incflo_correct_small_cells(get_velocity_new(),
                               GetVecOfConstPtrs(u_mac), GetVecOfConstPtrs(v_mac),
                               GetVecOfConstPtrs(w_mac));
}
