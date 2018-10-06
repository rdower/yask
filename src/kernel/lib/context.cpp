/*****************************************************************************

YASK: Yet Another Stencil Kernel
Copyright (c) 2014-2018, Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to
deal in the Software without restriction, including without limitation the
rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
sell copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

* The above copyright notice and this permission notice shall be included in
  all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
IN THE SOFTWARE.

*****************************************************************************/

// This file contains implementations of StencilContext methods.
// Also see setup.cpp.

#include "yask_stencil.hpp"
using namespace std;

namespace yask {

    // APIs.
    // See yask_kernel_api.hpp.

#define GET_SOLN_API(api_name, expr, step_ok, domain_ok, misc_ok, prep_req) \
    idx_t StencilContext::api_name(const string& dim) const {           \
        if (prep_req && !rank_bb.bb_valid)                              \
            THROW_YASK_EXCEPTION("Error: '" #api_name \
                                 "()' called before calling 'prepare_solution()'"); \
        checkDimType(dim, #api_name, step_ok, domain_ok, misc_ok);      \
        return expr;                                                    \
    }
    GET_SOLN_API(get_num_ranks, _opts->_num_ranks[dim], false, true, false, false)
    GET_SOLN_API(get_overall_domain_size, overall_domain_sizes[dim], false, true, false, true)
    GET_SOLN_API(get_rank_domain_size, _opts->_rank_sizes[dim], false, true, false, false)
    GET_SOLN_API(get_region_size, _opts->_region_sizes[dim], true, true, false, false)
    GET_SOLN_API(get_block_size, _opts->_block_sizes[dim], true, true, false, false)
    GET_SOLN_API(get_first_rank_domain_index, rank_bb.bb_begin[dim], false, true, false, true)
    GET_SOLN_API(get_last_rank_domain_index, rank_bb.bb_end[dim] - 1, false, true, false, true)
    GET_SOLN_API(get_min_pad_size, _opts->_min_pad_sizes[dim], false, true, false, false)
    GET_SOLN_API(get_rank_index, _opts->_rank_indices[dim], false, true, false, true)
#undef GET_SOLN_API

    // The grid sizes updated any time these settings are changed.
#define SET_SOLN_API(api_name, expr, step_ok, domain_ok, misc_ok, reset_prep) \
    void StencilContext::api_name(const string& dim, idx_t n) {         \
        checkDimType(dim, #api_name, step_ok, domain_ok, misc_ok);      \
        expr;                                                           \
        update_grid_info();                                             \
        if (reset_prep) rank_bb.bb_valid = ext_bb.bb_valid = false;     \
    }
    SET_SOLN_API(set_rank_index, _opts->_rank_indices[dim] = n, false, true, false, true)
    SET_SOLN_API(set_num_ranks, _opts->_num_ranks[dim] = n, false, true, false, true)
    SET_SOLN_API(set_rank_domain_size, _opts->_rank_sizes[dim] = n, false, true, false, true)
    SET_SOLN_API(set_region_size, _opts->_region_sizes[dim] = n, true, true, false, true)
    SET_SOLN_API(set_block_size, _opts->_block_sizes[dim] = n, true, true, false, true)
    SET_SOLN_API(set_min_pad_size, _opts->_min_pad_sizes[dim] = n, false, true, false, false)
#undef SET_SOLN_API

    void StencilContext::share_grid_storage(yk_solution_ptr source) {
        auto sp = dynamic_pointer_cast<StencilContext>(source);
        assert(sp);

        for (auto gp : gridPtrs) {
            auto gname = gp->get_name();
            auto si = sp->gridMap.find(gname);
            if (si != sp->gridMap.end()) {
                auto sgp = si->second;
                gp->share_storage(sgp);
            }
        }
    }

    string StencilContext::apply_command_line_options(const string& args) {

        // Create a parser and add base options to it.
        CommandLineParser parser;
        _opts->add_options(parser);

        // Tokenize default args.
        vector<string> argsv;
        parser.set_args(args, argsv);

        // Parse cmd-line options, which sets values in settings.
        parser.parse_args("YASK", argsv);

        // Return any left-over strings.
        string rem;
        for (auto r : argsv) {
            if (rem.length())
                rem += " ";
            rem += r;
        }
        return rem;
    }

    ///// StencilContext functions:

    // Set debug output to cout if my_rank == msg_rank
    // or a null stream otherwise.
    ostream& StencilContext::set_ostr() {
        yask_output_factory yof;
        if (_env->my_rank == _opts->msg_rank)
            set_debug_output(yof.new_stdout_output());
        else
            set_debug_output(yof.new_null_output());
        assert(_ostr);
        return *_ostr;
    }

    ///// Top-level methods for evaluating reference and optimized stencils.

    // Eval stencil bundle(s) over grid(s) using reference scalar code.
    void StencilContext::run_ref(idx_t first_step_index,
                                 idx_t last_step_index) {
        run_time.start();
        ostream& os = get_ostr();
        auto& step_dim = _dims->_step_dim;
        auto step_posn = +Indices::step_posn;
        int ndims = _dims->_stencil_dims.getNumDims();

        // Determine step dir from order of first/last.
        idx_t step_dir = (last_step_index >= first_step_index) ? 1 : -1;
        
        // Find begin, step and end in step-dim.
        idx_t begin_t = first_step_index;
        idx_t step_t = step_dir; // always +/- 1 for ref run.
        assert(step_t);
        idx_t end_t = last_step_index + step_dir; // end is beyond last.

        // backward?
        if (step_t < 0) {
            begin_t = end_t + step_t;
            end_t = step_t;
        }

        // Begin & end tuples.
        // Based on rank bounding box, not extended
        // BB because we don't use wave-fronts in the ref code.
        IdxTuple begin(_dims->_stencil_dims);
        begin.setVals(rank_bb.bb_begin, false);
        begin[step_dim] = begin_t;
        IdxTuple end(_dims->_stencil_dims);
        end.setVals(rank_bb.bb_end, false);
        end[step_dim] = end_t;

        TRACE_MSG("run_ref: [" << begin.makeDimValStr() << " ... " <<
                  end.makeDimValStr() << ")");

        // Force region & block sizes to whole rank size so that scratch
        // grids will be large enough. Turn off any temporal blocking.
        _opts->_region_sizes.setValsSame(0);
        _opts->_block_sizes.setValsSame(0);
        _opts->adjustSettings(get_env());
        update_grid_info();

        // Copy these settings to packs and realloc scratch grids.
        for (auto& sp : stPacks)
            sp->getLocalSettings() = *_opts;
        allocScratchData(os);

        // Use only one set of scratch grids.
        int scratch_grid_idx = 0;

        // Indices to loop through.
        // Init from begin & end tuples.
        ScanIndices rank_idxs(*_dims, false, &rank_domain_offsets);
        rank_idxs.begin = begin;
        rank_idxs.end = end;

        // Set offsets in scratch grids.
        // Requires scratch grids to be allocated for whole
        // rank instead of smaller grid size.
        update_scratch_grid_info(scratch_grid_idx, rank_idxs.begin);

        // Initial halo exchange.
        // TODO: get rid of all halo exchanges in this function,
        // and calculate overall problem in one rank.
        exchange_halos();

        // Number of iterations to get from begin_t, stopping before end_t,
        // stepping by step_t.
        const idx_t num_t = abs(end_t - begin_t);
        for (idx_t index_t = 0; index_t < num_t; index_t++)
        {
            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * step_t);
            const idx_t stop_t = (step_t > 0) ?
                min(start_t + step_t, end_t) :
                max(start_t + step_t, end_t);

            // Set indices that will pass through generated code
            // because the step loop is coded here.
            rank_idxs.index[step_posn] = index_t;
            rank_idxs.start[step_posn] = start_t;
            rank_idxs.stop[step_posn] = stop_t;
            rank_idxs.step[step_posn] = step_t;

            // Loop thru bundles. We ignore bundle packs here
            // because packing bundles is an optional optimizations.
            for (auto* asg : stBundles) {

                // Scan through n-D space.
                TRACE_MSG("run_ref: step " << start_t <<
                          " in non-scratch bundle '" << asg->get_name());

                // Check step.
                if (check_step_conds && !asg->is_in_valid_step(start_t)) {
                    TRACE_MSG("run_ref: not valid for step " << start_t);
                    continue;
                }
                
                // Exchange all dirty halos.
                exchange_halos();

                // Find the bundles that need to be processed.
                // This will be the prerequisite scratch-grid
                // bundles plus this non-scratch group.
                auto sg_list = asg->get_reqd_bundles();

                // Loop through all the needed bundles.
                for (auto* sg : sg_list) {

                    // Indices needed for the generated misc loops.  Will normally be a
                    // copy of rank_idxs except when updating scratch-grids.
                    ScanIndices misc_idxs = sg->adjust_span(scratch_grid_idx, rank_idxs);
                    misc_idxs.step.setFromConst(1); // ensure unit step.

                    // Define misc-loop function.  Since step is always 1, we
                    // ignore misc_stop.  If point is in sub-domain for this
                    // bundle, then evaluate the reference scalar code.
                    // TODO: fix domain of scratch grids.
#define misc_fn(misc_idxs)   do {                                       \
                        if (sg->is_in_valid_domain(misc_idxs.start))    \
                            sg->calc_scalar(scratch_grid_idx, misc_idxs.start); \
                    } while(0)

                    // Scan through n-D space.
                    TRACE_MSG("run_ref: step " << start_t <<
                              " in bundle '" << sg->get_name() << "': [" <<
                              misc_idxs.begin.makeValStr(ndims) <<
                              " ... " << misc_idxs.end.makeValStr(ndims) << ")");
#include "yask_misc_loops.hpp"
#undef misc_fn
                } // needed bundles.

                // Mark grids that [may] have been written to.
                // Mark grids as dirty even if not actually written by this
                // rank. This is needed because neighbors will not know what
                // grids are actually dirty, and all ranks must have the same
                // information about which grids are possibly dirty.
                mark_grids_dirty(nullptr, start_t, stop_t);

            } // all bundles.

        } // iterations.
        steps_done += abs(end_t - begin_t);

        // Final halo exchange.
        exchange_halos();

        run_time.stop();
    }

    // Eval stencil bundle pack(s) over grid(s) using optimized code.
    void StencilContext::run_solution(idx_t first_step_index,
                                      idx_t last_step_index)
    {
        CONTEXT_VARS(this);
        run_time.start();

        // Determine step dir from order of first/last.
        idx_t step_dir = (last_step_index >= first_step_index) ? 1 : -1;
        
        // Find begin, step and end in step-dim.
        idx_t begin_t = first_step_index;

        // Step-size in step-dim is number of region steps.
        // Then, it is multipled by +/-1 to get proper direction.
        idx_t step_t = max(wf_steps, idx_t(1)) * step_dir;
        assert(step_t);
        idx_t end_t = last_step_index + step_dir; // end is beyond last.

        // Begin, end, step tuples.
        // Based on overall bounding box, which includes
        // any needed extensions for wave-fronts.
        IdxTuple begin(_dims->_stencil_dims);
        begin.setVals(ext_bb.bb_begin, false);
        begin[step_dim] = begin_t;
        IdxTuple end(_dims->_stencil_dims);
        end.setVals(ext_bb.bb_end, false);
        end[step_dim] = end_t;
        IdxTuple step(_dims->_stencil_dims);
        step.setVals(_opts->_region_sizes, false); // step by region sizes.
        step[step_dim] = step_t;

        TRACE_MSG("run_solution: [" <<
                  begin.makeDimValStr() << " ... " <<
                  end.makeDimValStr() << ") by " <<
                  step.makeDimValStr());
        if (!rank_bb.bb_valid)
            THROW_YASK_EXCEPTION("Error: run_solution() called without calling prepare_solution() first");
        if (ext_bb.bb_size < 1) {
            TRACE_MSG("nothing to do in solution");
            return;
        }

#ifdef MODEL_CACHE
        ostream& os = get_ostr();
        if (context.my_rank != context.msg_rank)
            cache_model.disable();
        if (cache_model.isEnabled())
            os << "Modeling cache...\n";
#endif

        // Adjust end points for overlapping regions due to wavefront angle.
        // For each subsequent time step in a region, the spatial location
        // of each block evaluation is shifted by the angle for each
        // bundle pack. So, the total shift in a region is the angle * num
        // packs * num timesteps. This assumes all bundle packs
        // are inter-dependent to find maximum extension. Actual required
        // size may be less, but this will just result in some calls to
        // calc_region() that do nothing.
        //
        // Conceptually (showing 2 ranks in t and x dims):
        // -----------------------------  t = rt ------------------------------
        //   \   | \     \     \|  \   |  .      |   / |  \     \     \|  \   |
        //    \  |  \     \     |   \  |  .      |  / \|   \     \     |   \  |
        //     \ |r0 \  r1 \ r2 |\ r3\ |  .      | /r0 | r1 \  r2 \ r3 |\ r4\ |
        //      \|    \     \   | \   \|  .      |/    |\    \     \   | \   \|
        // ------------------------------ t = 0 -------------------------------
        //       |   rank 0     |      |         |     |   rank 1      |      |
        // x = begin[x]       end[x] end[x]  begin[x] begin[x]       end[x] end[x]
        //     (rank)        (rank) (ext)     (ext)    (rank)       (rank) (adj)
        //
        //                      |XXXXXX|         |XXXXX|  <- redundant calculations.
        // XXXXXX|  <- areas outside of outer ranks not calculated ->  |XXXXXXX
        //
        if (wf_steps > 0) {
            for (auto& dim : _dims->_domain_dims.getDims()) {
                auto& dname = dim.getName();

                // The end should be adjusted only if an extension doesn't
                // exist.  Extentions exist between ranks, so additional
                // adjustments are only needed at the end of the right-most
                // rank in each dim.  See "(adj)" in diagram above.
                if (right_wf_exts[dname] == 0)
                    end[dname] += wf_shift_pts[dname];

                // Ensure only one region in this dim if the original size
                // covered the whole rank in this dim.
                if (_opts->_region_sizes[dname] >= _opts->_rank_sizes[dname])
                    step[dname] = end[dname] - begin[dname];

            }
            TRACE_MSG("run_solution: after adjustment for " << num_wf_shifts <<
                      " wave-front shift(s): [" <<
                      begin.makeDimValStr() << " ... " <<
                      end.makeDimValStr() << ") by " <<
                      step.makeDimValStr());
        }
        // At this point, 'begin' and 'end' should describe the *max* range
        // needed in the domain for this rank for the first time step.  At
        // any subsequent time step, this max may be shifted for temporal
        // wavefronts or blocking. Also, for each time step, the *actual*
        // range will be adjusted as needed before any actual stencil
        // calculations are made.

        // Indices needed for the 'rank' loops.
        ScanIndices rank_idxs(*_dims, true, &rank_domain_offsets);
        rank_idxs.begin = begin;
        rank_idxs.end = end;
        rank_idxs.step = step;

        // Make sure threads are set properly for a region.
        set_region_threads();

        // Initial halo exchange.
        exchange_halos();

        // Number of iterations to get from begin_t to end_t-1,
        // stepping by step_t.
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(step_t));
        for (idx_t index_t = 0; index_t < num_t; index_t++)
        {
            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * step_t);
            const idx_t stop_t = (step_t > 0) ?
                min(start_t + step_t, end_t) :
                max(start_t + step_t, end_t);
            idx_t this_num_t = abs(stop_t - start_t);

            // Set indices that will pass through generated code.
            rank_idxs.index[step_posn] = index_t;
            rank_idxs.start[step_posn] = start_t;
            rank_idxs.stop[step_posn] = stop_t;
            rank_idxs.step[step_posn] = step_t;

            // If no wave-fronts (default), loop through packs here, and do
            // only one pack at a time in calc_region(). This is similar to
            // loop in calc_rank_ref(), but with packs instead of bundles.
            if (wf_steps == 0) {

                // Loop thru packs.
                for (auto& bp : stPacks) {

                    // Check step.
                    if (check_step_conds && !bp->is_in_valid_step(start_t)) {
                        TRACE_MSG("run_solution: step " << start_t <<
                                  " not valid for pack '" <<
                                  bp->get_name() << "'");
                        continue;
                    }
                
                    // Make 2 passes. 1: compute data needed for MPI
                    // send and send that data. 2: compute remaining
                    // data and unpack received MPI data.
                    for (int pass = 0; pass < 2; pass++) {

                        // If there is an MPI interior defined, set
                        // the proper flags.
                        if (mpi_interior.bb_valid) {
                            if (pass == 0) {
                                do_mpi_exterior = true;
                                do_mpi_interior = false;
                            } else {
                                do_mpi_exterior = false;
                                do_mpi_interior = true;
                            }
                        } else {
                            do_mpi_exterior = true;
                            do_mpi_interior = true;

                            // Only 1 pass needed when needed when not
                            // overlapping comms and compute.
                            if (pass > 0)
                                break;
                        }
                        
                        // Include automatically-generated loop code that calls
                        // calc_region(bp) for each region.
                        TRACE_MSG("run_solution: step " << start_t <<
                                  " for pack '" << bp->get_name() << "'");
                        if (do_mpi_exterior)
                            TRACE_MSG(" within MPI exterior");
                        if (do_mpi_interior)
                            TRACE_MSG(" within MPI interior");
#include "yask_rank_loops.hpp"

                        // Do the appropriate steps for halo exchange.
                        exchange_halos();

                    } // passes.

                    // Set the flags back to default.
                    do_mpi_exterior = true;
                    do_mpi_interior = true;
                }
            }

            // If doing wave-fronts, must loop through all packs in
            // calc_region().
            // TODO: allow overlapped comms when the region covers the
            // whole rank domain, regardless of how many steps it covers.
            else {

                // Null ptr => Eval all stencil packs each time
                // calc_region() is called.
                BundlePackPtr bp;

                // Include automatically-generated loop code that calls
                // calc_region() for each region.
                TRACE_MSG("run_solution: steps [" << start_t <<
                          " ... " << stop_t << ")");
#include "yask_rank_loops.hpp"

                // Exchange dirty halo(s).
                exchange_halos();
            }

            // Overall steps.
            steps_done += this_num_t;

            // Count steps for each pack to properly account for
            // step conditions when using temporal tiling.
            for (auto& bp : stPacks) {
                idx_t num_pack_steps = 0;

                if (!check_step_conds)
                    num_pack_steps = this_num_t;
                else {

                    // Loop through each step.
                    assert(abs(step_dir) == 1);
                    for (idx_t t = start_t; t != stop_t; t += step_dir) {

                        // Check step cond for this t.
                        if (bp->is_in_valid_step(t))
                            num_pack_steps++;
                    }
                }

                // Count steps for this pack.
                bp->add_steps(num_pack_steps);
            }

            // Call the auto-tuner to evaluate these steps.
            eval_auto_tuner(this_num_t);

        } // step loop.

#ifdef MODEL_CACHE
        // Print cache stats, then disable.
        // Thus, cache is only modeled for first call.
        if (cache_model.isEnabled()) {
            os << "Done modeling cache...\n";
            cache_model.dumpStats();
            cache_model.disable();
        }
#endif
        run_time.stop();
    } // run_solution().

    // Calculate results within a region.  Each region is typically computed
    // in a separate OpenMP 'for' region.  In this function, we loop over
    // the time steps and bundle packs and evaluate a pack in each of
    // the blocks in the region.  If 'sel_bp' is null, eval all packs; else
    // eval only the one pointed to.
    void StencilContext::calc_region(BundlePackPtr& sel_bp,
                                     const ScanIndices& rank_idxs) {
        CONTEXT_VARS(this);
        TRACE_MSG("calc_region: region [" <<
                  rank_idxs.start.makeValStr(nsdims) << " ... " <<
                  rank_idxs.stop.makeValStr(nsdims) << ") within rank [" <<
                  rank_idxs.begin.makeValStr(nsdims) << " ... " <<
                  rank_idxs.end.makeValStr(nsdims) << ")" );

        // Track time (use "else" to avoid double-counting).
        if (do_mpi_exterior)
            ext_time.start();
        else if (do_mpi_interior)
            int_time.start();

        // Init region begin & end from rank start & stop indices.
        ScanIndices region_idxs(*_dims, true, &rank_domain_offsets);
        region_idxs.initFromOuter(rank_idxs);

        // Time range.
        // When doing WF rank tiling, this loop will step through
        // several time-steps in each region.
        // When also doing TB, it will step by the block steps.
        idx_t begin_t = region_idxs.begin[step_posn];
        idx_t end_t = region_idxs.end[step_posn];
        idx_t step_dir = (end_t >= begin_t) ? 1 : -1;
        idx_t step_t = max(tb_steps, idx_t(1)) * step_dir;
        assert(step_t);
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(step_t));

        // Time loop.
        idx_t shift_num = 0;
        for (idx_t index_t = 0; index_t < num_t; index_t++) {

            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * step_t);
            const idx_t stop_t = (step_t > 0) ?
                min(start_t + step_t, end_t) :
                max(start_t + step_t, end_t);

            // Set step indices that will pass through generated code.
            region_idxs.index[step_posn] = index_t;
            region_idxs.start[step_posn] = start_t;
            region_idxs.stop[step_posn] = stop_t;

            // If no temporal blocking (default), loop through packs here,
            // and do only one pack at a time in calc_block(). This is
            // similar to the code in run_solution() for WF.
            if (tb_steps == 0) {

                // Stencil bundle packs to evaluate at this time step.
                for (auto& bp : stPacks) {

                    // Not a selected bundle pack?
                    if (sel_bp && sel_bp != bp)
                        continue;

                    TRACE_MSG("calc_region: no TB; pack '" <<
                              bp->get_name() << "' in step(s) [" <<
                              start_t << " ... " << stop_t << ")");

                    // Check step.
                    if (check_step_conds && !bp->is_in_valid_step(start_t)) {
                        TRACE_MSG("calc_region: step " << start_t <<
                                  " not valid for pack '" << bp->get_name() << "'");
                        continue;
                    }

                    // Steps within a region are based on pack block sizes.
                    auto& settings = bp->getActiveSettings();
                    region_idxs.step = settings._block_sizes;
                    region_idxs.step[step_posn] = step_t;

                    // Groups in region loops are based on block-group sizes.
                    region_idxs.group_size = settings._block_group_sizes;

                    // Set region_idxs begin & end based on shifted rank
                    // start & stop (original region begin & end), rank
                    // boundaries, and pack BB. This will be the base of the
                    // region loops.
                    bool ok = shift_region(rank_idxs.start, rank_idxs.stop,
                                             shift_num, bp,
                                             region_idxs);

                    DOMAIN_VAR_LOOP(i, j) {
                        
                        // If there is only one blk in a region, make sure
                        // this blk fills this whole region.
                        if (settings._block_sizes[i] >= settings._region_sizes[i])
                            region_idxs.step[i] = region_idxs.end[i] - region_idxs.begin[i];
                    }
                
                    // Only need to loop through the span of the region if it is
                    // at least partly inside the extended BB. For overlapping
                    // regions, they may start outside the domain but enter the
                    // domain as time progresses and their boundaries shift. So,
                    // we don't want to return if this condition isn't met.
                    if (ok) {
                        idx_t phase = 0; // Only 1 phase w/o TB.

                        // Include automatically-generated loop code that
                        // calls calc_block() for each block in this region.
                        // Loops through x from begin_rx to end_rx-1;
                        // similar for y and z.  This code typically
                        // contains the outer OpenMP loop(s).
#include "yask_region_loops.hpp"
                    }

                    // Mark grids that [may] have been written to by this
                    // pack.  Only mark for exterior computation, because we
                    // don't care about blocks not needed for MPI sends.
                    // Mark grids as dirty even if not actually written by
                    // this rank, perhaps due to sub-domains. This is needed
                    // because neighbors will not know what grids are
                    // actually dirty, and all ranks must have the same
                    // information about which grids are possibly dirty.
                    // TODO: make this smarter to save unneeded MPI
                    // exchanges.
                    if (do_mpi_exterior)
                        mark_grids_dirty(bp, start_t, stop_t);

                    // Need to shift for next pack and/or time.
                    shift_num++;
                    
                } // stencil bundle packs.
            } // no temporal blocking.

            // If using TB, iterate thru steps in a WF and packs in calc_block().
            else {

                TRACE_MSG("calc_region: w/TB in step(s) [" <<
                          start_t << " ... " << stop_t << ")");

                // Null ptr => Eval all stencil packs each time
                // calc_block() is called.
                BundlePackPtr bp;

                // Steps within a region are based on rank block sizes.
                auto& settings = *_opts;
                region_idxs.step = settings._block_sizes;
                region_idxs.step[step_posn] = step_t;

                // Groups in region loops are based on block-group sizes.
                region_idxs.group_size = settings._block_group_sizes;

                // Set region_idxs begin & end based on shifted start & stop
                // and rank boundaries.  This will be the base of the region
                // loops.  NB: calc_block() doesn't need to know about the
                // *original* region begin & end.
                bool ok = shift_region(rank_idxs.start, rank_idxs.stop,
                                         shift_num, bp,
                                         region_idxs);

                DOMAIN_VAR_LOOP(i, j) {

                    // If there is only one blk in a region, make sure
                    // this blk fills this whole region.
                    if (settings._block_sizes[i] >= settings._region_sizes[i])
                        region_idxs.step[i] = region_idxs.end[i] - region_idxs.begin[i];
                }
                
                // To tesselate n-D domain space, we use n+1 distinct
                // "phases".  For example, 1-D TB uses "upward" triangles
                // and "downward" triangles. Threads must sync after every
                // phase. Thus, the phase loop is here around the generated
                // loops.
                idx_t nphases = nddims + 1; 
                if (ok) {
                    for (idx_t phase = 0; phase < nphases; phase++) {

                        // Call calc_block() on every block.  Only the shapes
                        // corresponding to the current 'phase' will be
                        // calculated.
#include "yask_region_loops.hpp"
                    }
                }
            
                // Loop thru stencil bundle packs that were evaluated in
                // these 'tb_steps' to increment shift & mark dirty grids.
                for (idx_t t = start_t; t != stop_t; t += step_dir) {
                    for (auto& bp : stPacks) {

                        // Check step.
                        if (check_step_conds && !bp->is_in_valid_step(t))
                            continue;

                        // One shift for each pack in each TB step.
                        shift_num++;

                        // Mark grids that [may] have been written to by this
                        // pack.
                        mark_grids_dirty(bp, t, t + step_dir);
                    }
                }
            } // with temporal blocking.
        } // time.

        if (do_mpi_exterior) {
            double ext_delta = ext_time.stop();
            TRACE_MSG("secs spent in this region for rank-exterior blocks: " << makeNumStr(ext_delta));
        }
        else if (do_mpi_interior) {
            double int_delta = int_time.stop();
            TRACE_MSG("secs spent in this region for rank-interior blocks: " << makeNumStr(int_delta));
        }

    } // calc_region.
    
    // Calculate results within a block. This function calls
    // 'calc_mini_block()' for the specified pack or all packs if 'sel_bp'
    // is null.  When using TB, only the shape(s) needed for the tesselation
    // 'phase' are computed.  Typically called by a top-level OMP thread
    // from calc_region().
    void StencilContext::calc_block(BundlePackPtr& sel_bp,
                                    idx_t phase,
                                    const ScanIndices& region_idxs) {

        CONTEXT_VARS(this);
        auto* bp = sel_bp.get();
        int thread_idx = omp_get_thread_num();
        TRACE_MSG("calc_block: phase " << phase << ", block [" <<
                  region_idxs.start.makeValStr(nsdims) << " ... " <<
                  region_idxs.stop.makeValStr(nsdims) << 
                  ") within region [" <<
                  region_idxs.begin.makeValStr(nsdims) << " ... " <<
                  region_idxs.end.makeValStr(nsdims) << 
                  ") by thread " << thread_idx);

        // If we are not calculating some of the blocks, determine
        // whether this block is *completely* inside the interior.
        // A block even partially in the exterior is not considered
        // "inside".
        if (!do_mpi_interior || !do_mpi_exterior) {
            assert(do_mpi_interior || do_mpi_exterior);
            assert(mpi_interior.bb_valid);

            // Starting point and ending point must be in BB.
            bool inside = true;
            DOMAIN_VAR_LOOP(i, j) {

                // Starting before beginning of interior?
                if (region_idxs.start[i] < mpi_interior.bb_begin[j]) {
                    inside = false;
                    break;
                }

                // Stopping after ending of interior?
                if (region_idxs.stop[i] > mpi_interior.bb_end[j]) {
                    inside = false;
                    break;
                }
            }
            if (do_mpi_interior) {
                if (inside)
                    TRACE_MSG(" calculating because block is interior");
                else {
                    TRACE_MSG(" *not* calculating because block is exterior");
                    return;
                }
            }
            if (do_mpi_exterior) {
                if (!inside)
                    TRACE_MSG(" calculating because block is exterior");
                else {
                    TRACE_MSG(" *not* calculating because block is interior");
                    return;
                }
            }
        }

        // Init block begin & end from region start & stop indices.
        ScanIndices block_idxs(*_dims, true, 0);
        block_idxs.initFromOuter(region_idxs);

        // Time range.
        // When not doing TB, there is only one step.
        // When doing TB, we will only do one iteration here
        // that covers all steps,
        // and calc_mini_block() will loop over all steps.
        idx_t begin_t = block_idxs.begin[step_posn];
        idx_t end_t = block_idxs.end[step_posn];
        idx_t step_dir = (end_t >= begin_t) ? 1 : -1;
        idx_t step_t = max(tb_steps, idx_t(1)) * step_dir;
        assert(step_t);
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(step_t));

        // If TB is not being used, just process the given pack.
        // No need for a time loop.
        // No need to check bounds, because they were checked in
        // calc_region() when not using TB.
        if (tb_steps == 0) {
            assert(bp);
            assert(abs(step_t) == 1);
            assert(abs(end_t - begin_t) == 1);
            assert(num_t == 1);
        
            // Set step indices that will pass through generated code.
            block_idxs.index[step_posn] = 0;
            block_idxs.start[step_posn] = begin_t;
            block_idxs.stop[step_posn] = end_t;

            // Steps within a block are based on pack mini-block sizes.
            auto& settings = bp->getActiveSettings();
            block_idxs.step = settings._mini_block_sizes;
            block_idxs.step[step_posn] = step_t;
        
            // Groups in block loops are based on mini-block-group sizes.
            block_idxs.group_size = settings._mini_block_group_sizes;

            // Default settings for no TB.
            BundlePackPtr bp = sel_bp;
            idx_t nphases = 1;
            assert(phase == 0);
            idx_t nshapes = 1;
            idx_t shape = 0;
            int dims_to_bridge[phase];
            idx_t shift_num = 0;
            ScanIndices adj_block_idxs = block_idxs;

            // Include automatically-generated loop code that
            // calls calc_mini_block() for each mini-block in this block.
#include "yask_block_loops.hpp"
        } // no TB.

        // If TB is active, loop thru each required shape.
        else {

            // Recalc number of phases.
            idx_t nphases = nddims + 1; // E.g., nphases = 3 for 2D.
            assert(phase >= 0);
            assert(phase < nphases); // E.g., phase = 0..2 for 2D.
            
            // Determine number of shapes for this 'phase'. First and last
            // phase need one shape. Other (bridge) phases need one shape
            // for each combination of domain dims. E.g., need 'x' and
            // 'y' bridges for 2D problem in phase 1.
            idx_t nshapes = choose(nddims, phase);
            int dims_to_bridge[phase];

            // Set temporal indices to full range.
            block_idxs.index[step_posn] = 0; // only one index.
            block_idxs.start[step_posn] = begin_t;
            block_idxs.stop[step_posn] = end_t;

            // Steps within a block are based on rank mini-block sizes.
            auto& settings = *_opts;
            block_idxs.step = settings._mini_block_sizes;
            block_idxs.step[step_posn] = step_dir;

            // Groups in block loops are based on mini-block-group sizes.
            block_idxs.group_size = settings._mini_block_group_sizes;

            // Increase range of block to cover all phases and
            // shapes.
            ScanIndices adj_block_idxs = block_idxs;
            DOMAIN_VAR_LOOP(i, j) {
                    
                // TB shapes can extend to the right only.  They can
                // cover a range as big as this block's base plus the
                // next block in all dims, so we add the width of the
                // current block to the end.  This makes the adjusted
                // blocks overlap, but the size of each mini-block is
                // trimmed at each step to the proper active size.
                // TODO: find a way to make this more efficient to avoid
                // calling calc_mini_block() many times with nothing to
                // do.
                auto width = region_idxs.stop[i] - region_idxs.start[i];
                adj_block_idxs.end[i] += width;

                // If there is only one MB in this dim, stretch it to
                // fill the whole adjusted block.
                if (settings._mini_block_sizes[i] >= settings._block_sizes[i])
                    adj_block_idxs.step[i] = adj_block_idxs.end[i] - adj_block_idxs.begin[i];
            }
            TRACE_MSG("calc_block: phase " << phase <<
                      ", adjusted block [" <<
                      adj_block_idxs.begin.makeValStr(nsdims) << " ... " <<
                      adj_block_idxs.end.makeValStr(nsdims) << 
                      ") with mini-block stride " <<
                      adj_block_idxs.step.makeValStr(nsdims));
                    
            // Loop thru shapes.
            for (idx_t shape = 0; shape < nshapes; shape++) {

                // Get 'shape'th combo of 'phase' things from 'nddims'.
                // These will be used to create bridge shapes.
                combination(dims_to_bridge, nddims, phase, shape + 1);

                // Can only be one time iteration here when doing TB
                // because mini-block temporal size is always same
                // as block temporal size.
                assert(num_t == 1);
                    
                // Include automatically-generated loop code that calls
                // calc_mini_block() for each mini-block in this block.
                // NB: each starting block will have the *original*
                // begin & end indices, regardless of 'shift_num'.
                BundlePackPtr bp; // null.
#include "yask_block_loops.hpp"

            } // shape loop.
        } // TB.
    } // calc_block().

    // Calculate results within a mini-block.
    // This function calls 'StencilBundleBase::calc_mini_block()'
    // for each bundle in the specified pack or all packs if 'sel_bp' is
    // null. When using TB, only the 'shape' needed for the tesselation
    // 'phase' are computed. The starting 'shift_num' is relative
    // to the bottom of the current region and block.
    void StencilContext::calc_mini_block(BundlePackPtr& sel_bp,
                                         idx_t nphases, idx_t phase,
                                         idx_t nshapes, idx_t shape,
                                         int dims_to_bridge[],
                                         const ScanIndices& base_region_idxs,
                                         const ScanIndices& base_block_idxs,
                                         const ScanIndices& adj_block_idxs) {

        CONTEXT_VARS(this);
        int thread_idx = omp_get_thread_num();
        TRACE_MSG("calc_mini_block: phase " << phase <<
                  ", shape " << shape <<
                  ", mini-block [" <<
                  adj_block_idxs.start.makeValStr(nsdims) << " ... " <<
                  adj_block_idxs.stop.makeValStr(nsdims) << ") within base-block [" <<
                  base_block_idxs.begin.makeValStr(nsdims) << " ... " <<
                  base_block_idxs.end.makeValStr(nsdims) << ") within base-region [" <<
                  base_region_idxs.begin.makeValStr(nsdims) << " ... " <<
                  base_region_idxs.end.makeValStr(nsdims) << ")");

        // Hack to promote forward progress in MPI when calc'ing
        // interior only.
        // We do this only on thread 0 to avoid stacking up useless
        // MPI requests by many threads.
        if (do_mpi_interior && !do_mpi_exterior && thread_idx == 0)
            exchange_halos(true);

        // Init mini-block begin & end from blk start & stop indices.
        ScanIndices mini_block_idxs(*_dims, true, 0);
        mini_block_idxs.initFromOuter(adj_block_idxs);

        // Time range.
        // No more temporal blocks below mini-blocks, so we always step
        // by +/- 1.
        idx_t begin_t = mini_block_idxs.begin[step_posn];
        idx_t end_t = mini_block_idxs.end[step_posn];
        idx_t step_dir = (end_t >= begin_t) ? 1 : -1;
        idx_t step_t = 1 * step_dir;        // +/- 1.
        assert(step_t);
        const idx_t num_t = CEIL_DIV(abs(end_t - begin_t), abs(step_t));

        // Time loop.
        idx_t shift_num = 0;
        for (idx_t index_t = 0; index_t < num_t; index_t++) {

            // This value of index_t steps from start_t to stop_t-1.
            const idx_t start_t = begin_t + (index_t * step_t);
            const idx_t stop_t = (step_t > 0) ?
                min(start_t + step_t, end_t) :
                max(start_t + step_t, end_t);
            TRACE_MSG("calc_mini_block: phase " << phase <<
                      ", shape " << shape <<
                      ", in step " << start_t);
            assert(abs(stop_t - start_t) == 1); // no more TB.
            
            // Set step indices that will pass through generated code.
            mini_block_idxs.index[step_posn] = index_t;
            mini_block_idxs.begin[step_posn] = start_t;
            mini_block_idxs.end[step_posn] = stop_t;
            mini_block_idxs.start[step_posn] = start_t;
            mini_block_idxs.stop[step_posn] = stop_t;

            // Stencil bundle packs to evaluate at this time step.
            for (auto& bp : stPacks) {
            
                // Not a selected bundle pack?
                if (sel_bp && sel_bp != bp)
                    continue;
            
                // Check step.
                if (check_step_conds && !bp->is_in_valid_step(start_t)) {
                    TRACE_MSG("calc_mini_block: step " << start_t <<
                              " not valid for pack '" <<
                              bp->get_name() << "'");
                    continue;
                }
                TRACE_MSG("calc_mini_block: phase " << phase <<
                          ", shape " << shape <<
                          ", step " << start_t <<
                          ", pack '" << bp->get_name() <<
                          "', shift-num " << shift_num);

                // Start timers for this pack.
                // Tracking only on thread 0. It might be better to track
                // all threads and average them. Or something like that.
                if (thread_idx == 0)
                    bp->start_timers();
                
                // Steps within a mini-blk are based on sub-blk sizes.
                auto& settings = bp->getActiveSettings();
                mini_block_idxs.step = settings._sub_block_sizes;
                mini_block_idxs.step[step_posn] = step_t;

                // Groups in mini-blk loops are based on sub-block-group sizes.
                mini_block_idxs.group_size = settings._sub_block_group_sizes;

                // Set mini_block_idxs begin & end based on shifted begin &
                // end of block for given phase & shape.  This will be the
                // base for the mini-block loops, which have no temporal
                // tiling.
                bool ok =
                    shift_mini_block(adj_block_idxs.start, adj_block_idxs.stop,
                                     shift_num,
                                     adj_block_idxs.begin, adj_block_idxs.end,
                                     base_block_idxs.begin, base_block_idxs.end,
                                     shift_num,
                                     nphases, phase,
                                     nshapes, shape,
                                     dims_to_bridge,
                                     base_region_idxs.begin, base_region_idxs.end,
                                     shift_num,
                                     bp,
                                     mini_block_idxs);
                
                // Loop through bundles in this pack to do actual calcs.
                if (ok) {
                    for (auto* sb : *bp)
                        if (sb->getBB().bb_num_points)
                            sb->calc_mini_block(mini_block_idxs);
                }

                // Need to shift for next pack and/or time-step.
                shift_num++;

                // Stop timers for this pack.
                if (thread_idx == 0)
                    bp->stop_timers();

            } // packs.
        } // time.
    } // calc_mini_block().

    // Find boundaries within region with 'base_start' to 'base_stop'
    // shifted 'shift_num' times, which should start at 0 and increment for
    // each pack in each time-step.  Trim to ext-BB of 'bp' if not null.
    // Write results into 'begin' and 'end' in 'idxs'.  Return 'true' if
    // resulting area is non-empty, 'false' if empty.
    bool StencilContext::shift_region(const Indices& base_start, const Indices& base_stop,
                                        idx_t shift_num,
                                        BundlePackPtr& bp,
                                        ScanIndices& idxs) {
        CONTEXT_VARS(this);

        // For wavefront adjustments, see conceptual diagram in
        // run_solution().  At each pack and time-step, the parallelogram
        // may be trimmed based on the BB and WF extensions outside of the
        // rank-BB.

        // Actual region boundaries must stay within [extended] pack BB.
        // We have to calculate the posn in the extended rank at each
        // value of 'shift_num' because it is being shifted spatially.
        bool ok = true;
        DOMAIN_VAR_LOOP(i, j) {
            auto angle = wf_angles[j];

            // Shift initial spatial region boundaries for this iteration of
            // temporal wavefront.  Between regions, we only shift left, so
            // region loops must strictly increment. They may do so in any
            // order.  Shift by pts in one WF step.  Always shift left in
            // WFs.
            idx_t rstart = base_start[i] - angle * shift_num;
            idx_t rstop = base_stop[i] - angle * shift_num;

            // Trim to extended BB of pack if given.
            // Note that BBs are indexed by 'j' because they don't
            // contain step indices.
            if (bp) {
                auto& pbb = bp->getBB();
                rstart = max(rstart, pbb.bb_begin[j]);
                rstop = min(rstop, pbb.bb_end[j]);
            }

            // Find non-extended domain. We'll use this to determine if
            // we're in an extension, where special rules apply.
            idx_t dbegin = rank_bb.bb_begin[j];
            idx_t dend = rank_bb.bb_end[j];

            // In left ext, add 'angle' points for every shift to get
            // region boundary in ext.
            if (rstart < dbegin && left_wf_exts[j])
                rstart = max(rstart, dbegin - left_wf_exts[j] + shift_num * angle);

            // In right ext, subtract 'angle' points for every shift.
            if (rstop > dend && right_wf_exts[j])
                rstop = min(rstop, dend + right_wf_exts[j] - shift_num * angle);

            // Copy result into idxs.
            idxs.begin[i] = rstart;
            idxs.end[i] = rstop;

            // Anything to do in the adjusted region?
            if (rstop <= rstart)
                ok = false;
        }
        TRACE_MSG("shift_region: updated span: [" <<
                  idxs.begin.makeValStr(nsdims) << " ... " <<
                  idxs.end.makeValStr(nsdims) << ") within region base [" <<
                  base_start.makeValStr(nsdims) << " ... " <<
                  base_stop.makeValStr(nsdims) << ") shifted " <<
                  shift_num << " time(s) is " <<
                  (ok ? "not " : "") << "empty");
        return ok;
    }
    
    // For given 'phase' and 'shape', find boundaries within mini-block at
    // 'mb_base_start' to 'mb_base_stop' shifted by 'mb_shift_num', which
    // should start at 0 and increment for each pack in each time-step.
    // 'mb_base' is subset of 'adj_block_base'.
    // Trim to block at 'block_base_start' to 'block_base_stop' shifted by
    // 'block_shift_num'.  Trim to region at 'region_base_start' to
    // 'region_base_stop' shifted by 'region_shift_num'.  Trim to ext-BB of
    // 'bp' or rank if null.  Write results into 'begin' and 'end' in
    // 'idxs'.  Return 'true' if resulting area is non-empty, 'false' if
    // empty.
    bool StencilContext::shift_mini_block(const Indices& mb_base_start,
                                          const Indices& mb_base_stop,
                                          idx_t mb_shift_num,
                                          const Indices& adj_block_base_start,
                                          const Indices& adj_block_base_stop,
                                          const Indices& block_base_start,
                                          const Indices& block_base_stop,
                                          idx_t block_shift_num,
                                          idx_t nphases, idx_t phase,
                                          idx_t nshapes, idx_t shape,
                                          int dims_to_bridge[],
                                          const Indices& region_base_start,
                                          const Indices& region_base_stop,
                                          idx_t region_shift_num,
                                          BundlePackPtr& bp,
                                          ScanIndices& idxs) {
        CONTEXT_VARS(this);

        // Set 'idxs' begin & end to region boundaries for
        // given shift.
        bool ok = shift_region(region_base_start, region_base_stop,
                               region_shift_num,
                               bp,
                               idxs);

        // Loop thru dims, breaking out if any dim has no work.
        DOMAIN_VAR_LOOP(i, j) {

            // Determine range of this block for current phase, shape, and
            // shift. For each dim, we'll first compute the L & R sides of
            // the base block and the L side of the next block.

            // Is this block first and/or last in region?
            bool is_first_blk = block_base_start[i] <= region_base_start[i];
            bool is_last_blk = block_base_stop[i] >= region_base_stop[i];

            // Is there only one blk in the region in this dim?
            bool is_one_blk = is_first_blk && is_last_blk;

            // Initial start and stop point of phase-0 block.
            idx_t blk_start = block_base_start[i];
            idx_t blk_stop = block_base_stop[i];

            //   x->
            // ^   ----------------------
            // |  /        \            /^
            // t /  phase 0 \ phase 1  / |
            //  /            \        /  |
            //  ----------------------   |
            //  ^             ^       ^  |
            //  |<-blk_width->|    -->|  |<--sa=shifts*angle
            //  |             |    next_blk_start
            // blk_start  blk_stop    |
            //  |<-----blk_base------>|
            // blk_width = blk_base/2 + sa.

            // When there is >1 phase, initial width is half of base plus
            // one shift distance.  This will make 'up' and 'down'
            // trapezoids approx same size.
            // TODO: use actual number of shifts instead of max.
            auto tb_angle = tb_angles[j];
            if (nphases > 1 && !is_one_blk) {
                auto sa = (num_tb_shifts+1) * tb_angle;
                idx_t blk_width = ROUND_UP(CEIL_DIV(blk_stop - blk_start, idx_t(2)) + sa,
                                           fold_pts[j]);
                blk_width = max(blk_width, 2 * sa + fold_pts[j]);
                blk_stop = min(blk_start + blk_width, block_base_stop[i]);
            }

            // Starting point of the *next* block.  This is used to create
            // bridge shapes between blocks.  Initially, the beginning of
            // the next block is the end of this block.
            // TODO: split these parts more evenly when not full triangles.
            idx_t next_blk_start = block_base_stop[i];

            // Adjust these based on current shift.  Adjust by pts in one TB
            // step, reducing size on R & L sides.  But if block is first
            // and/or last, clamp to region.  TODO: have different R & L
            // angles. TODO: have different shifts for each pack.

            // Shift start to right unless first.  First block will be a
            // parallelogram or trapezoid clamped to beginning of region.
            blk_start += tb_angle * block_shift_num;
            if (is_first_blk)
                blk_start = idxs.begin[i];

            // Shift stop to left. If there will be no bridges, clamp
            // last block to end of region.
            blk_stop -= tb_angle * block_shift_num;
            if ((nphases == 1 || is_one_blk) && is_last_blk)
                blk_stop = idxs.end[i];

            // Shift start of next block. Last block will be
            // clamped to end of region.
            next_blk_start += tb_angle * block_shift_num;
            if (is_last_blk)
                next_blk_start = idxs.end[i];

            // Use these 3 values to determine the beginning and end
            // of the current shape for the current phase.
            // For phase 0, limits are simply the base start and stop.
            idx_t shape_start = blk_start;
            idx_t shape_stop = blk_stop;

            // Depending on the phase and shape, create a bridge between
            // from RHS of base block to the LHS of the next block
            // until all dims are bridged at last phase.
            if (phase > 0) {

                // Check list of dims to bridge for this shape,
                // computed earlier.
                for (int i = 0; i < phase; i++) {
                    auto dim = dims_to_bridge[i] - 1;

                    // Bridge this dim?
                    if (dim == j) {
                        TRACE_MSG("shift_mini_block: phase " << phase <<
                                  ", shape " << shape <<
                                  ": bridging dim " << j);
                
                        // Start at end of base block, but not
                        // before start of block.
                        shape_start = max(blk_stop, blk_start);
                    
                        // Stop at beginning of next block.
                        shape_stop = next_blk_start;
                    }
                }
            }
            // We now have bounds of this shape in shape_{start,stop}
            // for given phase and shift.
            if (shape_stop <= shape_start)
                ok = false;
            if (ok) {

                // Is this mini-block first and/or last in block?
                bool is_first_mb = mb_base_start[i] <= adj_block_base_start[i];
                bool is_last_mb = mb_base_stop[i] >= adj_block_base_stop[i];

                // Is there only one MB?
                bool is_one_mb = is_first_mb && is_last_mb;

                // Beginning and end of min-block.
                idx_t mb_start = mb_base_start[i];
                idx_t mb_stop = mb_base_stop[i];

                // Shift mini-block by MB angles unless there is only one.
                // MB is a wave-front, so only shift left.
                if (!is_one_mb) {
                    auto mb_angle = mb_angles[j];
                    mb_start -= mb_angle * mb_shift_num;
                    mb_stop -= mb_angle * mb_shift_num;
                }

                // Clamp first & last MB to shape boundaries.
                if (is_first_mb)
                    mb_start = shape_start;
                if (is_last_mb)
                    mb_stop = shape_stop;

                // Trim mini-block to fit in region.
                mb_start = max(mb_start, idxs.begin[i]);
                mb_stop = min(mb_stop, idxs.end[i]);
            
                // Trim mini-block range to fit in shape.
                mb_start = max(mb_start, shape_start);
                mb_stop = min(mb_stop, shape_stop);

                // Update 'idxs'.
                idxs.begin[i] = mb_start;
                idxs.end[i] = mb_stop;

                // No work to do?
                if (mb_stop <= mb_start)
                    ok = false;
            }

        } // dims.

        TRACE_MSG("shift_mini_block: phase " << phase << "/" << nphases <<
                  ", shape " << shape << "/" << nshapes <<
                  ", pack '" << bp->get_name() <<
                  ", updated span: [" <<
                  idxs.begin.makeValStr(nsdims) << " ... " <<
                  idxs.end.makeValStr(nsdims) << ") from original mini-block [" <<
                  mb_base_start.makeValStr(nsdims) << " ... " <<
                  mb_base_stop.makeValStr(nsdims) << ") shifted " <<
                  mb_shift_num << " time(s) within adj-block base [" <<
                  adj_block_base_start.makeValStr(nsdims) << " ... " <<
                  adj_block_base_stop.makeValStr(nsdims) << ") and actual block base [" <<
                  block_base_start.makeValStr(nsdims) << " ... " <<
                  block_base_stop.makeValStr(nsdims) << ") shifted " <<
                  block_shift_num << " time(s) and region base [" <<
                  region_base_start.makeValStr(nsdims) << " ... " <<
                  region_base_stop.makeValStr(nsdims) << ") shifted " <<
                  region_shift_num << " time(s) is " <<
                  (ok ? "not " : "") << "empty");
        return ok;
    }
    
    // Eval auto-tuner for given number of steps.
    void StencilContext::eval_auto_tuner(idx_t num_steps) {
        _at.steps_done += num_steps;

        if (_use_pack_tuners) {
            for (auto& sp : stPacks)
                sp->getAT().eval();
        }
        else
            _at.eval();
    }
    
    // Reset auto-tuners.
    void StencilContext::reset_auto_tuner(bool enable, bool verbose) {
        for (auto& sp : stPacks)
            sp->getAT().clear(!enable, verbose);
        _at.clear(!enable, verbose);
    }

    // Determine if any auto tuners are running.
    bool StencilContext::is_auto_tuner_enabled() const {
        bool done = true;
        if (_use_pack_tuners) {
            for (auto& sp : stPacks)
                if (!sp->getAT().is_done())
                    done = false;
        } else
            done = _at.is_done();
        return !done;
    }
    
    // Apply auto-tuning immediately, i.e., not as part of normal processing.
    // Will alter data in grids.
    void StencilContext::run_auto_tuner_now(bool verbose) {
        if (!rank_bb.bb_valid)
            THROW_YASK_EXCEPTION("Error: run_auto_tuner_now() called without calling prepare_solution() first");
        ostream& os = get_ostr();

        os << "Auto-tuning...\n" << flush;
        YaskTimer at_timer;
        at_timer.start();

        // Temporarily disable halo exchange to tune intra-rank.
        enable_halo_exchange = false;

        // Temporarily ignore step conditions to force eval of conditional
        // bundles.  NB: may affect perf, e.g., if packs A and B run in
        // AAABAAAB sequence, perf may be [very] different if run as
        // ABABAB..., esp. w/temporal tiling.  TODO: work around this.
        check_step_conds = false;

        // Init tuners.
        reset_auto_tuner(true, verbose);

        // Reset stats.
        clear_timers();

        // Determine number of steps to run.
        // If wave-fronts are enabled, run a max number of these steps.
        idx_t region_steps = _opts->_region_sizes[_dims->_step_dim];
        idx_t step_dir = _dims->_step_dir; // +/- 1.
        idx_t step_t = min(max(region_steps, idx_t(1)), +AutoTuner::max_step_t) * step_dir;

        // Run time-steps until AT converges.
        for (idx_t t = 0; ; t += step_t) {

            // Run step_t time-step(s).
            run_solution(t, t + step_t - step_dir);

            // AT done on this rank?
            if (!is_auto_tuner_enabled())
                break;
        }

        // Wait for all ranks to finish.
        os << "Waiting for auto-tuner to converge on all ranks...\n";
        _env->global_barrier();

        // reenable normal operation.
#ifndef NO_HALO_EXCHANGE
        enable_halo_exchange = true;
#endif
        check_step_conds = true;

        // Report results.
        at_timer.stop();
        os << "Auto-tuner done after " << steps_done << " step(s) in " <<
            at_timer.get_elapsed_secs() << " secs.\n";
        if (_use_pack_tuners) {
            for (auto& sp : stPacks)
                sp->getAT().print_settings(os);
        } else
            _at.print_settings(os);
        print_temporal_tiling_info();

        // Reset stats.
        clear_timers();
    }

    // Add a new grid to the containers.
    void StencilContext::addGrid(YkGridPtr gp, bool is_output) {
        assert(gp);
        auto& gname = gp->get_name();
        if (gridMap.count(gname))
            THROW_YASK_EXCEPTION("Error: grid '" + gname + "' already exists");

        // Add to list and map.
        gridPtrs.push_back(gp);
        gridMap[gname] = gp;

        // Add to output list and map if 'is_output'.
        if (is_output) {
            outputGridPtrs.push_back(gp);
            outputGridMap[gname] = gp;
        }
    }

    // Adjust offsets of scratch grids based on thread number 'thread_idx'
    // and beginning point of block 'idxs'.  Each scratch-grid is assigned
    // to a thread, so it must "move around" as the thread is assigned to
    // each block.  This move is accomplished by changing the grids' global
    // and local offsets.
    void StencilContext::update_scratch_grid_info(int thread_idx,
                                                  const Indices& idxs) {
        auto& dims = get_dims();
        int nsdims = dims->_stencil_dims.size();
        auto step_posn = Indices::step_posn;

        // Loop thru vecs of scratch grids.
        for (auto* sv : scratchVecs) {
            assert(sv);

            // Get ptr to the scratch grid for this thread.
            auto gp = sv->at(thread_idx);
            assert(gp);
            assert(gp->is_scratch());

            // i: index for stencil dims, j: index for domain dims.
            DOMAIN_VAR_LOOP(i, j) {

                auto& dim = dims->_stencil_dims.getDim(i);
                auto& dname = dim.getName();

                // Is this dim used in this grid?
                int posn = gp->get_dim_posn(dname);
                if (posn >= 0) {

                    // | ... |        +------+       |
                    // |  global ofs  |      |       |
                    // |<------------>|grid/ |       |
                    // |     |  loc   | blk  |       |
                    // |rank |  ofs   |domain|       |
                    // | ofs |<------>|      |       |
                    // |<--->|        +------+       |
                    // ^     ^        ^              ^
                    // |     |        |              last rank-domain index
                    // |     |        start of grid-domain/0-idx of block
                    // |     first rank-domain index
                    // first overall-domain index

                    // Local offset is the offset of this grid
                    // relative to the current rank.
                    // Set local offset to diff between global offset
                    // and rank offset.
                    // Round down to make sure it's vec-aligned.
                    auto rofs = rank_domain_offsets[j];
                    auto vlen = gp->_get_vec_len(posn);
                    auto lofs = round_down_flr(idxs[i] - rofs, vlen);
                    gp->_set_local_offset(posn, lofs);

                    // Set global offset of grid based on starting point of block.
                    // This is a global index, so it will include the rank offset.
                    // Thus, it it not necessarily a vec mult.
                    // Need to use calculated local offset to adjust for any
                    // rounding that was done above.
                    gp->_set_offset(posn, rofs + lofs);
                }
            }
        }
    }

    static void print_pct(ostream& os, double ntime, double dtime) {
        if (dtime > 0.) {
            float pct = 100. * ntime / dtime;
            os << " (" << pct << "%)";
        }
        os << endl;
    }

    /// Get statistics associated with preceding calls to run_solution().
    yk_stats_ptr StencilContext::get_stats() {
        ostream& os = get_ostr();

        // Calc times.
        double rtime = run_time.get_elapsed_secs();
        double htime = min(halo_time.get_elapsed_secs(), rtime);
        double wtime = min(wait_time.get_elapsed_secs(), htime);
        double etime = min(ext_time.get_elapsed_secs(), rtime - htime);
        double itime = min(int_time.get_elapsed_secs(), rtime - htime - etime);
        double ctime = etime + itime;
        double otime = max(rtime - ctime - htime, 0.);

        // Init return object.
        auto p = make_shared<Stats>();
        p->npts = tot_domain_pts; // NOT sum over steps.
        p->nsteps = steps_done;
        p->run_time = rtime;
        p->halo_time = htime;
        p->nreads = 0;
        p->nwrites = 0;
        p->nfpops = 0;
        p->pts_ps = 0.;
        p->reads_ps = 0.;
        p->writes_ps = 0.;
        p->flops = 0.;

        // Sum work done across packs using per-pack step counters.
        double tptime = 0.;
        double optime = 0.;
        idx_t psteps = 0;
        for (auto& sp : stPacks) {

            // steps in this pack.
            idx_t ns = sp->steps_done;

            auto& ps = sp->stats;
            ps.nsteps = ns;
            ps.npts = tot_domain_pts; // NOT sum over steps.
            ps.nreads = sp->tot_reads_per_step * ns;
            ps.nwrites = sp->tot_writes_per_step * ns;
            ps.nfpops = sp->tot_fpops_per_step * ns;

            // Add to total work.
            psteps += ns;
            p->nreads += ps.nreads;
            p->nwrites += ps.nwrites;
            p->nfpops += ps.nfpops;

            // Adjust pack time to make sure total time is <= compute time.
            double ptime = sp->timer.get_elapsed_secs();
            ptime = min(ptime, ctime - tptime);
            tptime += ptime;
            ps.run_time = ptime;
            ps.halo_time = 0.;

            // Pack rates.
            idx_t np = tot_domain_pts * ns; // Sum over steps.
            ps.reads_ps = 0.;
            ps.writes_ps = 0.;
            ps.flops = 0.;
            ps.pts_ps = 0.;
            if (ptime > 0.) {
                ps.reads_ps = ps.nreads / ptime;
                ps.writes_ps = ps.nwrites / ptime;
                ps.flops = ps.nfpops / ptime;
                ps.pts_ps = np / ptime;
            }
        }
        optime = max(ctime - tptime, 0.); // remaining time.

        // Overall rates.
        idx_t npts_done = tot_domain_pts * steps_done;
        if (rtime > 0.) {
            p->reads_ps= double(p->nreads) / rtime;
            p->writes_ps= double(p->nwrites) / rtime;
            p->flops = double(p->nfpops) / rtime;
            p->pts_ps = double(npts_done) / rtime;
        }
      
        if (steps_done > 0) {
            os <<
                "\nWork stats:\n"
                " num-steps-done:                   " << makeNumStr(steps_done) << endl <<
                " num-reads-per-step:               " << makeNumStr(double(p->nreads) / steps_done) << endl <<
                " num-writes-per-step:              " << makeNumStr(double(p->nwrites) / steps_done) << endl <<
                " num-est-FP-ops-per-step:          " << makeNumStr(double(p->nfpops) / steps_done) << endl <<
                " num-points-per-step:              " << makeNumStr(tot_domain_pts) << endl;
            if (psteps != steps_done) {
                os <<
                    " Work breakdown by stencil pack(s):\n";
                for (auto& sp : stPacks) {
                    idx_t ns = sp->steps_done;
                    idx_t nreads = sp->tot_reads_per_step;
                    idx_t nwrites = sp->tot_writes_per_step;
                    idx_t nfpops = sp->tot_fpops_per_step;
                    string pfx = "  '" + sp->get_name() + "' ";
                    os << pfx << "num-steps-done:           " << makeNumStr(ns) << endl <<
                        pfx << "num-reads-per-step:       " << makeNumStr(nreads) << endl <<
                        pfx << "num-writes-per-step:      " << makeNumStr(nwrites) << endl <<
                        pfx << "num-est-FP-ops-per-step:  " << makeNumStr(nfpops) << endl;
                }
            }
            os << 
                "\nTime stats:\n"
                " elapsed-time (sec):               " << makeNumStr(rtime) << endl <<
                " Time breakdown by activity type:\n"
                "  compute time (sec):                " << makeNumStr(ctime);
            print_pct(os, ctime, rtime);
#ifdef USE_MPI
            os <<
                "  halo exchange time (sec):          " << makeNumStr(htime);
            print_pct(os, htime, rtime);
#endif
            os <<
                "  other time (sec):                  " << makeNumStr(otime);
            print_pct(os, otime, rtime);
            if (psteps != steps_done) {
                os <<
                    " Compute-time breakdown by stencil pack(s):\n";
                for (auto& sp : stPacks) {
                    auto& ps = sp->stats;
                    double ptime = ps.run_time;
                    string pfx = "  '" + sp->get_name() + "' ";
                    os << pfx << "time (sec):       " << makeNumStr(ptime);
                    print_pct(os, ptime, ctime);
                }
                os << "  other (sec):                       " << makeNumStr(optime);
                print_pct(os, optime, ctime);
            }
#ifdef USE_MPI
            os <<
                " Compute-time breakdown by halo area:\n"
                "  rank-exterior compute (sec):       " << makeNumStr(etime);
            print_pct(os, etime, ctime);
            os <<
                "  rank-interior compute (sec):       " << makeNumStr(itime);
            print_pct(os, itime, ctime);
            os <<
                " Halo-time breakdown:\n"
                "  MPI waits (sec):                   " << makeNumStr(wtime);
            print_pct(os, wtime, htime);
            double ohtime = max(htime - wtime, 0.);
            os <<
                "  packing, unpacking, etc. (sec):    " << makeNumStr(ohtime);
            print_pct(os, ohtime, htime);
#endif
            os <<
                "\nRate stats:\n"
                " throughput (num-reads/sec):       " << makeNumStr(p->reads_ps) << endl <<
                " throughput (num-writes/sec):      " << makeNumStr(p->writes_ps) << endl <<
                " throughput (est-FLOPS):           " << makeNumStr(p->flops) << endl <<
                " throughput (num-points/sec):      " << makeNumStr(p->pts_ps) << endl;
            if (psteps != steps_done) {
                os <<
                    " Rate breakdown by stencil pack(s):\n";
                for (auto& sp : stPacks) {
                    auto& ps = sp->stats;
                    string pfx = "  '" + sp->get_name() + "' ";
                    os <<
                        pfx << "throughput (num-reads/sec):   " << makeNumStr(ps.reads_ps) << endl <<
                        pfx << "throughput (num-writes/sec):  " << makeNumStr(ps.writes_ps) << endl <<
                        pfx << "throughput (est-FLOPS):       " << makeNumStr(ps.flops) << endl <<
                        pfx << "throughput (num-points/sec):  " << makeNumStr(ps.pts_ps) << endl;
                    
                }
            }
        }

        // Clear counters.
        clear_timers();

        return p;
    }

    // Compare grids in contexts.
    // Return number of mis-compares.
    idx_t StencilContext::compareData(const StencilContext& ref) const {
        ostream& os = get_ostr();

        os << "Comparing grid(s) in '" << name << "' to '" << ref.name << "'..." << endl;
        if (gridPtrs.size() != ref.gridPtrs.size()) {
            cerr << "** number of grids not equal." << endl;
            return 1;
        }
        idx_t errs = 0;
        for (size_t gi = 0; gi < gridPtrs.size(); gi++) {
            TRACE_MSG("Grid '" << ref.gridPtrs[gi]->get_name() << "'...");
            errs += gridPtrs[gi]->compare(ref.gridPtrs[gi].get());
        }

        return errs;
    }

    // Exchange dirty halo data for all grids and all steps.
    void StencilContext::exchange_halos(bool test_only) {

#ifdef USE_MPI
        if (!enable_halo_exchange || _env->num_ranks < 2)
            return;

        halo_time.start();
        double wait_delta = 0.;
        TRACE_MSG("exchange_halos");
        if (test_only)
            TRACE_MSG(" testing only");
        else {
            if (do_mpi_exterior)
                TRACE_MSG(" following calc of MPI exterior");
            if (do_mpi_interior)
                TRACE_MSG(" following calc of MPI interior");
        }
        auto opts = get_settings();
        auto& sd = _dims->_step_dim;

        // Vars for list of grids that need to be swapped and their step indices.
        GridPtrMap gridsToSwap;
        map<string, vector_set<idx_t>> stepsToSwap;
        int num_swaps = 0;
        size_t max_steps = 0;

        // TODO: move this into a separate function.
        if (test_only) {
            int num_tests = 0;

            // Call MPI_Test() on all unfinished requests to promote MPI progress.
            // TODO: replace with more direct and less intrusive techniques.
            
            // Loop thru MPI data.
            for (auto& mdi : mpiData) {
                auto& gname = mdi.first;
                auto& grid_mpi_data = mdi.second;
                MPI_Request* grid_recv_reqs = grid_mpi_data.recv_reqs.data();
                MPI_Request* grid_send_reqs = grid_mpi_data.send_reqs.data();

                int flag;
                for (size_t i = 0; i < grid_mpi_data.recv_reqs.size(); i++) {
                    auto& r = grid_recv_reqs[i];
                    if (r != MPI_REQUEST_NULL) {
                        //TRACE_MSG(gname << " recv test &MPI_Request = " << &r);
                        MPI_Test(&r, &flag, MPI_STATUS_IGNORE);
                        num_tests++;
                    }
                }
                for (size_t i = 0; i < grid_mpi_data.send_reqs.size(); i++) {
                    auto& r = grid_send_reqs[i];
                    if (r != MPI_REQUEST_NULL) {
                        //TRACE_MSG(gname << " send test &MPI_Request = " << &r);
                        MPI_Test(&r, &flag, MPI_STATUS_IGNORE);
                        num_tests++;
                    }
                }
            }
            TRACE_MSG("exchange_halos: " << num_tests << " MPI test(s) issued");
        }

        else {

            // Loop thru all bundle packs.
            // TODO: do this only once per step.
            // TODO: expand this to hold misc indices also.  Use an ordered map
            // by name to make sure grids are in same order on all ranks.
            for (auto& bp : stPacks) {

                // Loop thru stencil bundles in this pack.
                for (auto* sg : *bp) {

                    // Find the bundles that need to be processed.
                    // This will be any prerequisite scratch-grid
                    // bundles plus this non-scratch bundle.
                    // We need to loop thru the scratch-grid
                    // bundles so we can consider the inputs
                    // to them for exchanges.
                    auto sg_list = sg->get_reqd_bundles();

                    // Loop through all the needed bundles.
                    for (auto* csg : sg_list) {

                        TRACE_MSG("exchange_halos: checking " << csg->inputGridPtrs.size() <<
                                  " input grid(s) to bundle '" << csg->get_name() <<
                                  "' that is needed for bundle '" << sg->get_name() << "'");

                        // Loop thru all *input* grids in this bundle.
                        for (auto gp : csg->inputGridPtrs) {

                            // Don't swap scratch grids.
                            if (gp->is_scratch())
                                continue;

                            // Only need to swap grids that have any MPI buffers.
                            auto& gname = gp->get_name();
                            if (mpiData.count(gname) == 0)
                                continue;

                            // Check all allocated step indices.
                            idx_t start = 0, stop = 1;
                            if (gp->is_dim_used(sd)) {
                                start = min(start, gp->_get_first_alloc_index(sd));
                                stop = max(stop, gp->_get_last_alloc_index(sd) + 1);
                            }
                            for (idx_t t = start; t < stop; t++) {
                            
                                // Only need to swap grids whose halos are not up-to-date
                                // for this step.
                                if (!gp->is_dirty(t))
                                    continue;

                                // Swap this grid.
                                gridsToSwap[gname] = gp;
                                stepsToSwap[gname].insert(t);
                                num_swaps++;
                                max_steps = max(max_steps, stepsToSwap[gname].size());

                                // Cannot swap >1 step if overlapping comms/calc
                                // because we only have one step buffer per grid.
                                // TODO: fix this.
                                if (!do_mpi_exterior || !do_mpi_interior)
                                    assert(stepsToSwap[gname].size() == 1);

                            } // steps.
                        } // grids.
                    } // needed bundles.
                } // bundles in pack.
            } // packs.
            TRACE_MSG("exchange_halos: need to exchange halos for " <<
                      num_swaps << " steps(s) in " <<
                      gridsToSwap.size() << " grid(s)");
            assert(gridsToSwap.size() == stepsToSwap.size());
        }

        // Loop thru step-vector indices.
        // This loop is outside because we only have one buffer
        // per grid. Thus, we have to complete comms before
        // transerring another step. TODO: fix this.
        for (size_t svi = 0; svi < max_steps; svi++) {

            // Sequence of things to do for each grid's neighbors.
            enum halo_steps { halo_irecv, halo_pack_isend, halo_unpack, halo_final };
            vector<halo_steps> steps_to_do;

            // Flags indicate what part of grids were most recently calc'd.
            if (do_mpi_exterior) {
                steps_to_do.push_back(halo_irecv);
                steps_to_do.push_back(halo_pack_isend);
            }
            if (do_mpi_interior) {
                steps_to_do.push_back(halo_unpack);
                steps_to_do.push_back(halo_final);
            }
            int num_send_reqs = 0;
            int num_recv_reqs = 0;
            for (auto halo_step : steps_to_do) {

                if (halo_step == halo_irecv)
                    TRACE_MSG("exchange_halos: requesting data phase");
                else if (halo_step == halo_pack_isend)
                    TRACE_MSG("exchange_halos: packing and sending data phase");
                else if (halo_step == halo_unpack)
                    TRACE_MSG("exchange_halos: waiting for and unpacking data phase");
                else if (halo_step == halo_final)
                    TRACE_MSG("exchange_halos: waiting for send to finish phase");
                else
                    THROW_YASK_EXCEPTION("internal error: unknown halo-exchange step");

                // Loop thru all grids to swap.
                // Use 'gi' as an MPI tag.
                int gi = 0;
                for (auto gtsi : gridsToSwap) {
                    gi++;
                    auto& gname = gtsi.first;
                    auto gp = gtsi.second;
                    auto& grid_mpi_data = mpiData.at(gname);
                    MPI_Request* grid_recv_reqs = grid_mpi_data.recv_reqs.data();
                    MPI_Request* grid_send_reqs = grid_mpi_data.send_reqs.data();

                    // Get needed step in this grid.
                    auto& steps = stepsToSwap[gname];
                    if (steps.size() <= svi)
                        continue; // no step at this index.
                    idx_t si = steps.at(svi);
                    TRACE_MSG(" for grid '" << gname << "' w/step-index " << si);

                    // Loop thru all this rank's neighbors.
                    grid_mpi_data.visitNeighbors
                        ([&](const IdxTuple& offsets, // NeighborOffset.
                             int neighbor_rank,
                             int ni, // unique neighbor index.
                             MPIBufs& bufs) {
                            auto& sendBuf = bufs.bufs[MPIBufs::bufSend];
                            auto& recvBuf = bufs.bufs[MPIBufs::bufRecv];
                            TRACE_MSG("  with rank " << neighbor_rank << " at relative position " <<
                                      offsets.subElements(1).makeDimValOffsetStr());

                            // Submit async request to receive data from neighbor.
                            if (halo_step == halo_irecv) {
                                auto nbytes = recvBuf.get_bytes();
                                if (nbytes) {
                                    void* buf = (void*)recvBuf._elems;
                                    TRACE_MSG("   requesting " << makeByteStr(nbytes));
                                    auto& r = grid_recv_reqs[ni];
                                    //TRACE_MSG(gname << " Irecv &MPI_Request = " << &r);
                                    MPI_Irecv(buf, nbytes, MPI_BYTE,
                                              neighbor_rank, int(gi),
                                              _env->comm, &r);
                                    num_recv_reqs++;
                                }
                                else
                                    TRACE_MSG("   0B to request");
                            }

                            // Pack data into send buffer, then send to neighbor.
                            else if (halo_step == halo_pack_isend) {
                                auto nbytes = sendBuf.get_bytes();
                                if (nbytes) {

                                    // Vec ok?
                                    // Domain sizes must be ok, and buffer size must be ok
                                    // as calculated when buffers were created.
                                    bool send_vec_ok = allow_vec_exchange && sendBuf.vec_copy_ok;

                                    // Get first and last ranges.
                                    IdxTuple first = sendBuf.begin_pt;
                                    IdxTuple last = sendBuf.last_pt;

                                    // The code in allocMpiData() pre-calculated the first and
                                    // last points of each buffer, except in the step dim.
                                    // So, we need to set that value now.
                                    // TODO: update this if we expand the buffers to hold
                                    // more than one step.
                                    if (gp->is_dim_used(sd)) {
                                        first.setVal(sd, si);
                                        last.setVal(sd, si);
                                    }
                                    TRACE_MSG("   packing " << sendBuf.num_pts.makeDimValStr(" * ") <<
                                              " points from [" << first.makeDimValStr() <<
                                              " ... " << last.makeDimValStr() << ") " <<
                                              (send_vec_ok ? "with" : "without") <<
                                              " vector copy");

                                    // Copy (pack) data from grid to buffer.
                                    void* buf = (void*)sendBuf._elems;
                                    if (send_vec_ok)
                                        gp->get_vecs_in_slice(buf, first, last);
                                    else
                                        gp->get_elements_in_slice(buf, first, last);

                                    // Send packed buffer to neighbor.
                                    auto nbytes = sendBuf.get_bytes();
                                    TRACE_MSG("   sending " << makeByteStr(nbytes));
                                    auto& r = grid_send_reqs[ni];
                                    //TRACE_MSG(gname << " Isend &MPI_Request = " << &r);
                                    MPI_Isend(buf, nbytes, MPI_BYTE,
                                              neighbor_rank, int(gi), _env->comm, &r);
                                    num_send_reqs++;
                                }
                                else
                                    TRACE_MSG("   0B to send");
                            }

                            // Wait for data from neighbor, then unpack it.
                            else if (halo_step == halo_unpack) {
                                auto nbytes = recvBuf.get_bytes();
                                if (nbytes) {

                                    // Wait for data from neighbor before unpacking it.
                                    auto& r = grid_recv_reqs[ni];
                                    //TRACE_MSG(gname << " recv wait &MPI_Request = " << &r);
                                    if (r != MPI_REQUEST_NULL) {
                                        TRACE_MSG("   waiting for receipt of " << makeByteStr(nbytes));
                                        wait_time.start();
                                        MPI_Wait(&r, MPI_STATUS_IGNORE);
                                        wait_delta += wait_time.stop();
                                    }

                                    // Vec ok?
                                    bool recv_vec_ok = allow_vec_exchange && recvBuf.vec_copy_ok;

                                    // Get first and last ranges.
                                    IdxTuple first = recvBuf.begin_pt;
                                    IdxTuple last = recvBuf.last_pt;

                                    // Set step val as above.
                                    if (gp->is_dim_used(sd)) {
                                        first.setVal(sd, si);
                                        last.setVal(sd, si);
                                    }
                                    TRACE_MSG("   got data; unpacking " << recvBuf.num_pts.makeDimValStr(" * ") <<
                                              " points into [" << first.makeDimValStr() <<
                                              " ... " << last.makeDimValStr() << ") " <<
                                              (recv_vec_ok ? "with" : "without") <<
                                              " vector copy");

                                    // Copy data from buffer to grid.
                                    void* buf = (void*)recvBuf._elems;
                                    idx_t n = 0;
                                    if (recv_vec_ok)
                                        n = gp->set_vecs_in_slice(buf, first, last);
                                    else
                                        n = gp->set_elements_in_slice(buf, first, last);
                                    assert(n == recvBuf.get_size());
                                }
                                else
                                    TRACE_MSG("   0B to wait for");
                            }

                            // Final steps.
                            else if (halo_step == halo_final) {
                                auto nbytes = sendBuf.get_bytes();
                                if (nbytes) {

                                    // Wait for send to finish.
                                    // TODO: consider using MPI_WaitAll.
                                    // TODO: strictly, we don't have to wait on the
                                    // send to finish until we want to reuse this buffer,
                                    // so we could wait on the *previous* send right before
                                    // doing another one.
                                    auto& r = grid_send_reqs[ni];
                                    //TRACE_MSG(gname << " send wait &MPI_Request = " << &r);
                                    if (r != MPI_REQUEST_NULL) {
                                        TRACE_MSG("   waiting to finish send of " << makeByteStr(nbytes));
                                        wait_time.start();
                                        MPI_Wait(&grid_send_reqs[ni], MPI_STATUS_IGNORE);
                                        wait_delta += wait_time.stop();
                                    }
                                }

                                // Mark grids as up-to-date when done.
                                if (gp->is_dirty(si)) {
                                    gp->set_dirty(false, si);
                                    TRACE_MSG("grid '" << gname <<
                                              "' marked as clean at step-index " << si);
                                }
                            }
                            
                        }); // visit neighbors.

                } // grids.
            } // exchange sequence.

            TRACE_MSG("exchange_halos: " << num_recv_reqs << " MPI receive request(s) issued");
            TRACE_MSG("exchange_halos: " << num_send_reqs << " MPI send request(s) issued");

        } // step indices.

        auto mpi_call_time = halo_time.stop();
        TRACE_MSG("exchange_halos: secs spent in MPI waits: " << makeNumStr(wait_delta));
        TRACE_MSG("exchange_halos: secs spent in this call: " << makeNumStr(mpi_call_time));
#endif
    }

    // Mark grids that have been written to by bundle pack 'sel_bp'.
    // TODO: only mark grids that are written to in their halo-read area.
    // TODO: add index for misc dim(s).
    // TODO: track sub-domain of grid that is dirty.
    void StencilContext::mark_grids_dirty(const BundlePackPtr& sel_bp,
                                          idx_t start, idx_t stop) {
        idx_t step = (start > stop) ? -1 : 1;
        map<YkGridPtr, set<idx_t>> grids_done;

        // Stencil bundle packs.
        for (auto& bp : stPacks) {

            // Not a selected bundle pack?
            if (sel_bp && sel_bp != bp)
                continue;

            // Each input step.
            for (idx_t t = start; t != stop; t += step) {

                // Each bundle in this pack.
                for (auto* sb : *bp) {

                    // Get output step for this bundle, if any.
                    // For many stencils, this will be t+1 or
                    // t-1 if stepping backward.
                    idx_t t_out = 0;
                    if (!sb->get_output_step_index(t, t_out))
                        continue;

                    // Output grids for this bundle.  NB: don't need to mark
                    // scratch grids as dirty because they are never exchanged.
                    for (auto gp : sb->outputGridPtrs) {

                        // Mark output step as dirty if not already done.
                        if (grids_done[gp].count(t_out) == 0) {
                            gp->set_dirty(true, t_out);
                            TRACE_MSG("grid '" << gp->get_name() <<
                                      "' marked as dirty at step " << t_out);
                            grids_done[gp].insert(t_out);
                        }
                    }
                }
            }
        }
    }

    // Reset elapsed times to zero.
    void StencilContext::clear_timers() {
        run_time.clear();
        ext_time.clear();
        int_time.clear();
        halo_time.clear();
        wait_time.clear();
        steps_done = 0;
        for (auto& sp : stPacks) {
            sp->timer.clear();
            sp->steps_done = 0;
        }
    }
    
} // namespace yask.
