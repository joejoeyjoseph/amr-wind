
#include <incflo.H>
#include <derive_F.H>
#include <setup_F.H>

// Need this for TagCutCells
#ifdef AMREX_USE_EB
#include <AMReX_EBAmrUtil.H>
#endif

// static variables of incflo class

int           incflo::ntrac = 1;
constexpr int incflo::nghost;
constexpr int incflo::nghost_for_slopes;
constexpr int incflo::nghost_for_bcs;

#ifdef AMREX_USE_EB
constexpr int incflo::m_eb_basic_grow_cells;
constexpr int incflo::m_eb_volume_grow_cells;
constexpr int incflo::m_eb_full_grow_cells;
#endif

// Constructor
// Note that geometry on all levels has already been defined in the AmrCore constructor,
// which the incflo class inherits from.
incflo::incflo ()
{
    // NOTE: Geometry on all levels has just been defined in the AmrCore
    // constructor. No valid BoxArray and DistributionMapping have been defined.
    // But the arrays for them have been resized.

    // Read inputs file using ParmParse
    ReadParameters();

#ifdef AMREX_USE_EB
    // This is needed before initializing level MultiFabs: ebfactories should
    // not change after the eb-dependent MultiFabs are allocated.
    MakeEBGeometry();
#endif

    // Initialize memory for data-array internals
    ResizeArrays();

    // Allocate the arrays for each face that will hold the bcs
    MakeBCArrays();

    // xxxxx flux registers
}

incflo::~incflo ()
{}

void incflo::InitData ()
{
    BL_PROFILE("incflo::InitData()");

    int restart_flag = 0;
    if(restart_file.empty())
    {
        // This tells the AmrMesh class not to iterate when creating the initial
        // grid hierarchy
        // SetIterateToFalse();

        // This tells the Cluster routine to use the new chopping routine which
        // rejects cuts if they don't improve the efficiency
        SetUseNewChop();

        // This is an AmrCore member function which recursively makes new levels
        // with MakeNewLevelFromScratch.
        InitFromScratch(cur_time);

        // xxxxx averagedown ???

        // xxxxx if (check_int > 0) { WriteCheckPointFile(); }
    }
    else
    {
        // Read starting configuration from chk file.
        ReadCheckpointFile();
    }

    // Post-initialisation step
    // - Initialize diffusive and projection operators
    // - Fill boundaries
    // - Create instance of MAC projection class
    // - Apply initial conditions
    // - Project initial velocity to make divergence free
    // - Perform dummy iterations to find pressure distribution
    PostInit(restart_flag);

    // Plot initial distribution
    if((plot_int > 0 || plot_per_exact > 0 || plot_per_approx > 0) && !restart_flag)
    {
        WritePlotFile();
        last_plt = 0;
    }
    if(KE_int > 0 && !restart_flag)
    {
        amrex::Print() << "Time, Kinetic Energy: " << cur_time << ", " << ComputeKineticEnergy() << std::endl;
    }

    ParmParse pp("incflo");
    bool write_eb_surface = 0;
    pp.query("write_eb_surface", write_eb_surface);

#ifdef AMREX_USE_EB
    if (write_eb_surface)
    {
        amrex::Print() << "Writing the geometry to a vtp file.\n" << std::endl;
        WriteMyEBSurface();
    }
#endif
}

void incflo::Evolve()
{
    BL_PROFILE("incflo::Evolve()");

    bool do_not_evolve = ((max_step == 0) || ((stop_time >= 0.) && (cur_time > stop_time)) ||
   					     ((stop_time <= 0.) && (max_step <= 0))) && !steady_state;

    while(!do_not_evolve)
    {
        // TODO: Necessary for dynamic meshing
        /* if (regrid_int > 0)
        {
            // Make sure we don't regrid on max_level
            for (int lev = 0; lev < max_level; ++lev)
            {
                // regrid is a member function of AmrCore
                if (nstep % regrid_int == 0)
                {
                    regrid(lev, time);
                    incflo_setup_solvers();
                }
         
            }
         
            if (nstep % regrid_int == 0)
            {
              setup_level_mask();
            }
         
        }*/

        // Advance to time t + dt
        Advance();
        nstep++;
        cur_time += dt;

        if (writeNow())
        {
            WritePlotFile();
            last_plt = nstep;
        }

        if(check_int > 0 && (nstep % check_int == 0))
        {
            WriteCheckPointFile();
            last_chk = nstep;
        }
        
        if(KE_int > 0 && (nstep % KE_int == 0))
        {
            amrex::Print() << "Time, Kinetic Energy: " << cur_time << ", " << ComputeKineticEnergy() << std::endl;
        }

        // Mechanism to terminate incflo normally.
        do_not_evolve = (steady_state && SteadyStateReached()) ||
                        ((stop_time > 0. && (cur_time >= stop_time - 1.e-12 * dt)) ||
                         (max_step >= 0 && nstep >= max_step));
    }

	// Output at the final time
    if( check_int > 0                                               && nstep != last_chk) WriteCheckPointFile();
    if( (plot_int > 0 || plot_per_exact > 0 || plot_per_approx > 0) && nstep != last_plt) WritePlotFile();
}

// tag cells for refinement
// overrides the pure virtual function in AmrCore
void incflo::ErrorEst(int lev,
                      TagBoxArray& tags,
                      Real time,
                      int ngrow)
{
    BL_PROFILE("incflo::ErrorEst()");

    const char   tagval = TagBox::SET;
    const char clearval = TagBox::CLEAR;

#ifdef AMREX_USE_EB
    auto const& factory = dynamic_cast<EBFArrayBoxFactory const&>(vel[lev]->Factory());
    auto const& flags = factory.getMultiEBCellFlagFab();
#endif

    const Real* dx      = geom[lev].CellSize();
    const Real* prob_lo = geom[lev].ProbLo();

#ifdef _OPENMP
#pragma omp parallel
#endif
    for (MFIter mfi(*vel[lev],true); mfi.isValid(); ++mfi)
    {
#ifdef AMREX_USE_EB
        const Box& bx  = mfi.tilebox();
        const auto& flag = flags[mfi];
        const FabType typ = flag.getType(bx);
        if (typ != FabType::covered)
        {
            TagBox&     tagfab  = tags[mfi];

            // tag cells for refinement
            state_error(BL_TO_FORTRAN_BOX(bx),
                        BL_TO_FORTRAN_ANYD(tagfab),
                        BL_TO_FORTRAN_ANYD((ebfactory[lev]->getVolFrac())[mfi]),
                        &tagval, &clearval,
                        AMREX_ZFILL(dx), AMREX_ZFILL(prob_lo), &time);
        }
#else
            TagBox&     tagfab  = tags[mfi];

            // tag cells for refinement
//          state_error(BL_TO_FORTRAN_BOX(bx),
//                      BL_TO_FORTRAN_ANYD(tagfab),
//                      BL_TO_FORTRAN_ANYD((ebfactory[lev]->getVolFrac())[mfi]),
//                      &tagval, &clearval,
//                      AMREX_ZFILL(dx), AMREX_ZFILL(prob_lo), &time);
#endif
    }

#ifdef AMREX_USE_EB
    refine_cutcells = true;
    // Refine on cut cells
    if (refine_cutcells)
    {
        amrex::TagCutCells(tags, *vel[lev]);
    }
#endif
}

// Make a new level from scratch using provided BoxArray and DistributionMapping.
// Only used during initialization.
// overrides the pure virtual function in AmrCore
void incflo::MakeNewLevelFromScratch(int lev,
                                     Real time,
                                     const BoxArray& new_grids,
                                     const DistributionMapping& new_dmap)
{
    BL_PROFILE("incflo::MakeNewLevelFromScratch()");

    if(incflo_verbose > 0)
    {
        amrex::Print() << "Making new level " << lev << std::endl;
        amrex::Print() << "with BoxArray " << new_grids << std::endl;
    }

    SetBoxArray(lev, new_grids);
    SetDistributionMap(lev, new_dmap);

#ifdef AMREX_USE_EB
    m_factory[lev] = makeEBFabFactory(geom[lev], grids[lev], dmap[lev],
                                      {m_eb_basic_grow_cells,
                                       m_eb_volume_grow_cells,
                                       m_eb_full_grow_cells},
                                       EBSupport::full);
#else
    m_factory[lev].reset(new FArrayBoxFactory());
#endif

    m_leveldata[lev].reset(new LevelData(grids[lev], dmap[lev], *m_factory[lev]));

    t_new[lev] = time;
    t_old[lev] = time - 1.e200;

    init_prob_fluid(lev);

    set_background_pressure(lev);

    amrex::Abort("xxxxx So far so good");
}

// Make a new level using provided BoxArray and DistributionMapping and
// fill with interpolated coarse level data.
// overrides the pure virtual function in AmrCore
void incflo::MakeNewLevelFromCoarse (int lev,
                                     Real time,
                                     const BoxArray& ba,
                                     const DistributionMapping& dm)
{
    BL_PROFILE("incflo::MakeNewLevelFromCoarse()");

    amrex::Print() << "ABORT: incflo::MakeNewLevelFromCoarse() not yet implemented. " << std::endl;
    amrex::Abort();
}

// Remake an existing level using provided BoxArray and DistributionMapping and
// fill with existing fine and coarse data.
// overrides the pure virtual function in AmrCore
void incflo::RemakeLevel (int lev, Real time, const BoxArray& ba,
			 const DistributionMapping& dm)
{
    BL_PROFILE("incflo::RemakeLevel()");

    amrex::Print() << "ABORT: incflo::RemakeLevel() not yet implemented. " << std::endl;
    amrex::Abort();
}

// Delete level data
// overrides the pure virtual function in AmrCore
void incflo::ClearLevel (int lev)
{
    BL_PROFILE("incflo::ClearLevel()");

    amrex::Print() << "ABORT: incflo::ClearLevel() not yet implemented. " << std::endl;
    amrex::Abort();
}

// Set covered coarse cells to be the average of overlying fine cells
// TODO: Move somewhere else, for example setup/incflo_arrays.cpp
void incflo::AverageDown()
{
    BL_PROFILE("incflo::AverageDown()");

    for (int lev = finest_level - 1; lev >= 0; --lev)
    {
        AverageDownTo(lev);
    }
}

void incflo::AverageDownTo(int crse_lev)
{
    BL_PROFILE("incflo::AverageDownTo()");

    IntVect rr = refRatio(crse_lev);

#ifdef AMREX_USE_EB
    amrex::EB_average_down(*vel[crse_lev+1],        *vel[crse_lev],        0, AMREX_SPACEDIM, rr);
    amrex::EB_average_down( *gp[crse_lev+1],         *gp[crse_lev],        0, AMREX_SPACEDIM, rr);

    if (!constant_density)
       amrex::EB_average_down(*density[crse_lev+1], *density[crse_lev],    0, 1, rr);

    if (advect_tracer)
       amrex::EB_average_down(*tracer[crse_lev+1],  *tracer[crse_lev],     0, ntrac, rr);

    amrex::EB_average_down(*eta[crse_lev+1],        *eta[crse_lev],        0, 1, rr);
    amrex::EB_average_down(*strainrate[crse_lev+1], *strainrate[crse_lev], 0, 1, rr);
    amrex::EB_average_down(*vort[crse_lev+1],       *vort[crse_lev],       0, 1, rr);
#else
    amrex::average_down(*vel[crse_lev+1],        *vel[crse_lev],        0, AMREX_SPACEDIM, rr);
    amrex::average_down( *gp[crse_lev+1],         *gp[crse_lev],        0, AMREX_SPACEDIM, rr);

    if (!constant_density)
       amrex::average_down(*density[crse_lev+1], *density[crse_lev],    0, 1, rr);

    if (advect_tracer)
       amrex::average_down(*tracer[crse_lev+1],  *tracer[crse_lev],     0, ntrac, rr);

    amrex::average_down(*eta[crse_lev+1],        *eta[crse_lev],        0, 1, rr);
    amrex::average_down(*strainrate[crse_lev+1], *strainrate[crse_lev], 0, 1, rr);
    amrex::average_down(*vort[crse_lev+1],       *vort[crse_lev],       0, 1, rr);
#endif
}

bool
incflo::writeNow()
{
    bool write_now = false;

    if ( plot_int > 0 && (nstep % plot_int == 0) ) 
        write_now = true;

    else if ( plot_per_exact  > 0 && (std::abs(remainder(cur_time, plot_per_exact)) < 1.e-12) ) 
        write_now = true;

    else if (plot_per_approx > 0.0)
    {
        // Check to see if we've crossed a plot_per_approx interval by comparing
        // the number of intervals that have elapsed for both the current
        // time and the time at the beginning of this timestep.

        int num_per_old = (cur_time-dt) / plot_per_approx;
        int num_per_new = (cur_time   ) / plot_per_approx;

        // Before using these, however, we must test for the case where we're
        // within machine epsilon of the next interval. In that case, increment
        // the counter, because we have indeed reached the next plot_per_approx interval
        // at this point.

        const Real eps = std::numeric_limits<Real>::epsilon() * 10.0 * std::abs(cur_time);
        const Real next_plot_time = (num_per_old + 1) * plot_per_approx;

        if ((num_per_new == num_per_old) && std::abs(cur_time - next_plot_time) <= eps)
        {
            num_per_new += 1;
        }

        // Similarly, we have to account for the case where the old time is within
        // machine epsilon of the beginning of this interval, so that we don't double
        // count that time threshold -- we already plotted at that time on the last timestep.

        if ((num_per_new != num_per_old) && std::abs((cur_time - dt) - next_plot_time) <= eps)
            num_per_old += 1;

        if (num_per_old != num_per_new)
            write_now = true;
    }

    return write_now;
}
