#include <fenv.h>
#include "defs.hpp"

#include "node_server.hpp"
#include "node_client.hpp"
#include "future.hpp"
#include <chrono>
#include <unistd.h>
#include <hpx/hpx_init.hpp>
#include "problem.hpp"

#include "options.hpp"
options opts;

bool gravity_on = true;
bool hydro_on = true;
HPX_PLAIN_ACTION(grid::set_pivot, set_pivot_action);

void compute_ilist();

void initialize(options _opts) {
	opts = _opts;
//#ifndef NDEBUG
	feenableexcept (FE_DIVBYZERO);
	feenableexcept (FE_INVALID);
	feenableexcept (FE_OVERFLOW);
//#endif
	grid::set_scaling_factor(opts.xscale);
	grid::set_max_level(opts.max_level);

	if (opts.problem == DWD) {
		set_problem(scf_binary);
		set_refine_test(refine_test_bibi);
	} else if (opts.problem == SOD) {
		grid::set_fgamma(7.0 / 5.0);
		gravity_on = false;
		set_problem(sod_shock_tube);
		set_refine_test (refine_sod);
		grid::set_analytic_func(sod_shock_tube);
	} else if (opts.problem == BLAST) {
		grid::set_fgamma(7.0 / 5.0);
		gravity_on = false;
		set_problem (blast_wave);
		set_refine_test (refine_blast);
	} else if (opts.problem == STAR) {
		grid::set_fgamma(5.0 / 3.0);
		set_problem(star);
		set_refine_test(refine_test_bibi);
	} else if (opts.problem == MOVING_STAR) {
		grid::set_fgamma(5.0 / 3.0);
		grid::set_analytic_func(moving_star_analytic);
		set_problem(moving_star);
		set_refine_test(refine_test_bibi);
		/*} else if (opts.problem == OLD_SCF) {
		 set_refine_test(refine_test_bibi);
		 set_problem(init_func_type([=](real a, real b, real c, real dx) {
		 return old_scf(a,b,c,opts.omega,opts.core_thresh_1,opts.core_thresh_2, dx);
		 }));
		 if (!opts.found_restart_file) {
		 if (opts.omega < ZERO) {
		 printf("Must specify omega for bibi polytrope\n");
		 throw;
		 }
		 if (opts.core_thresh_1 < ZERO) {
		 printf("Must specify core_thresh_1 for bibi polytrope\n");
		 throw;
		 }
		 if (opts.core_thresh_2 < ZERO) {
		 printf("Must specify core_thresh_2 for bibi polytrope\n");
		 throw;
		 }
		 }*/
	} else if (opts.problem == SOLID_SPHERE) {
		hydro_on = false;
		set_problem(init_func_type([](real x, real y, real z, real dx) {
			return solid_sphere(x,y,z,dx,0.25);
		}));
	} else {
		printf("No problem specified\n");
		throw;
	}
	node_server::set_gravity(gravity_on);
	node_server::set_hydro(hydro_on);
	compute_ilist();
}

HPX_PLAIN_ACTION(initialize, initialize_action);

real OMEGA;
void node_server::set_pivot() {
	auto localities = hpx::find_all_localities();
	space_vector pivot = grid_ptr->center_of_mass();
	std::vector<hpx::future<void>> futs;
	futs.reserve(localities.size());
	for (auto& locality : localities) {
		if (current_time == ZERO) {
			futs.push_back(hpx::async < set_pivot_action > (locality, pivot));
		}
	}
	for (auto&& fut : futs) {
		fut.get();
	}
}

int hpx_main(int argc, char* argv[]) {
	printf("Running\n");
	auto test_fut = hpx::async([]() {
//		while(1){hpx::this_thread::yield();}
	});
	test_fut.get();

	try {
		if (opts.process_options(argc, argv)) {

			auto all_locs = hpx::find_all_localities();
			std::vector<hpx::future<void>> futs;
            futs.reserve(all_locs.size());
			for (auto i = all_locs.begin(); i != all_locs.end(); ++i) {
				futs.push_back(hpx::async < initialize_action > (*i, opts));
			}
			for (auto i = futs.begin(); i != futs.end(); ++i) {
				i->get();
			}

			node_client root_id = hpx::new_ < node_server > (hpx::find_here());
			node_client root_client(root_id);

			if (opts.found_restart_file) {
				set_problem(null_problem);
				const std::string fname = opts.restart_filename;
				printf("Loading from %s...\n", fname.c_str());
				if (opts.output_only) {
					const std::string oname = opts.output_filename;
					root_client.get_ptr().get()->load_from_file_and_output(fname, oname);
				} else {
					root_client.get_ptr().get()->load_from_file(fname);
					root_client.regrid(root_client.get_gid(), true).get();
				}
				printf("Done. \n");
			} else {
				for (integer l = 0; l < opts.max_level; ++l) {
					root_client.regrid(root_client.get_gid(), false).get();
					printf("---------------Created Level %i---------------\n\n", int(l + 1));
				}
				root_client.regrid(root_client.get_gid(), false).get();
				printf("---------------Regridded Level %i---------------\n\n", int(opts.max_level));
			}

			std::vector < hpx::id_type > null_sibs(geo::direction::count());
			printf("Forming tree connections------------\n");
			root_client.form_tree(root_client.get_gid(), hpx::invalid_id, null_sibs).get();
			if (gravity_on) {
				//real tstart = MPI_Wtime();
				root_client.solve_gravity(false).get();
				//	printf("Gravity Solve Time = %e\n", MPI_Wtime() - tstart);
			}
			printf("...done\n");

			if (!opts.output_only) {
				//	set_problem(null_problem);
				root_client.start_run(opts.problem == DWD && !opts.found_restart_file).get();
			}

            root_client.report_timing();
		}
	} catch (...) {

	}
	printf("Exiting...\n");
	return hpx::finalize();
}

