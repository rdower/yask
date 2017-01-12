/*****************************************************************************

YASK: Yet Another Stencil Kernel
Copyright (c) 2014-2017, Intel Corporation

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

// Stencil types.
#include "stencil.hpp"

// Base classes for stencil code.
#include "stencil_calc.hpp"

using namespace std;

namespace yask {

    ///// StencilContext functions:

    // Init MPI, OMP, etc.
    void StencilContext::initEnv(int* argc, char*** argv)
    {
        // Stop collecting VTune data.
        // Even better to use -start-paused option.
        VTUNE_PAUSE;

        // MPI init.
        my_rank = 0;
        num_ranks = 1;
#ifdef USE_MPI
        int provided = 0;
        MPI_Init_thread(argc, argv, MPI_THREAD_SERIALIZED, &provided);
        if (provided < MPI_THREAD_SERIALIZED) {
            cerr << "error: MPI_THREAD_SERIALIZED not provided.\n";
            exit_yask(1);
        }
        comm = MPI_COMM_WORLD;
        MPI_Comm_rank(comm, &my_rank);
        MPI_Comm_size(comm, &num_ranks);
#else
        comm = 0;
#endif

        // Enable the default output stream on the msg-rank only.
        set_ostr();

        // There is no specific call to init OMP, but we make a gratuitous
        // OMP call to trigger any debug output.
        omp_get_num_procs();
        
        // Make sure any MPI/OMP debug data is dumped before continuing.
        global_barrier();
    }

    // Set ostr to given stream if provided.
    // If not provided, set to cout if my_rank == msg_rank
    // or a null stream otherwise.
    ostream& StencilContext::set_ostr(std::ostream* os) {
        if (os)
            _ostr = os;
        else if (my_rank == _opts->msg_rank)
            _ostr = &cout;
        else
            _ostr = new ofstream;    // null stream (unopened ofstream).
        assert(_ostr);
        return *_ostr;
    }
    
    
    ///// Top-level methods for evaluating reference and optimized stencils.

    // Eval stencil equation group(s) over grid(s) using scalar code.
    void StencilContext::calc_rank_ref()
    {
        idx_t begin_dt = ofs_t;
        idx_t end_dt = begin_dt + _opts->dt;
        TRACE_MSG("calc_rank_ref(%ld..%ld)", begin_dt, end_dt-1);
        
        // Time steps.
        // TODO: check that scalar version actually does CPTS_T time steps.
        // (At this point, CPTS_T == 1 for all existing stencil examples.)
        for(idx_t t = begin_dt; t < end_dt; t += CPTS_T) {

            // equations to evaluate (only one in most stencils).
            for (auto eg : eqGroups) {

                // Halo exchange for grid(s) updated by this equation.
                eg->exchange_halos(t, t + CPTS_T);

                // Loop through 4D space within the bounding-box of this
                // equation set.
#pragma omp parallel for collapse(4)
                for (idx_t n = eg->begin_bbn; n < eg->end_bbn; n++)
                    for (idx_t x = eg->begin_bbx; x < eg->end_bbx; x++)
                        for (idx_t y = eg->begin_bby; y < eg->end_bby; y++)
                            for (idx_t z = eg->begin_bbz; z < eg->end_bbz; z++) {

                                // Update only if point in domain for this eq group.
                                if (eg->is_in_valid_domain(t, n, x, y, z)) {
                                    
                                    TRACE_MSG("%s.calc_scalar(%ld, %ld, %ld, %ld, %ld)", 
                                              eg->get_name().c_str(), t, n, x, y, z);
                                        
                                    // Evaluate the reference scalar code.
                                    eg->calc_scalar(t, n, x, y, z);
                                }
                            }
            }
        } // iterations.
    }

    // Eval equation group(s) over grid(s) using optimized code.
    void StencilContext::calc_rank_opt()
    {
        idx_t begin_dt = ofs_t;
        idx_t end_dt = begin_dt + _opts->dt;
        idx_t step_dt = _opts->rt;
        TRACE_MSG("calc_rank_opt(%ld..%ld by %ld)", begin_dt, end_dt-1, step_dt);
        ostream& os = get_ostr();

#ifdef MODEL_CACHE
        if (context.my_rank != context.msg_rank)
            cache_model.disable();
        if (cache_model.isEnabled())
            os << "Modeling cache...\n";
#endif
        
        // Problem begin points.
        idx_t begin_dn = begin_bbn;
        idx_t begin_dx = begin_bbx;
        idx_t begin_dy = begin_bby;
        idx_t begin_dz = begin_bbz;
    
        // Problem end-points.
        idx_t end_dn = end_bbn;
        idx_t end_dx = end_bbx;
        idx_t end_dy = end_bby;
        idx_t end_dz = end_bbz;

        // Steps are based on region sizes.
        idx_t step_dn = _opts->rn;
        idx_t step_dx = _opts->rx;
        idx_t step_dy = _opts->ry;
        idx_t step_dz = _opts->rz;

        // Groups in rank loops are set to smallest size.
        const idx_t group_size_dn = 1;
        const idx_t group_size_dx = 1;
        const idx_t group_size_dy = 1;
        const idx_t group_size_dz = 1;

        // Extend end points for overlapping regions due to wavefront angle.
        // For each subsequent time step in a region, the spatial location of
        // each block evaluation is shifted by the angle for each stencil. So,
        // the total shift in a region is the angle * num stencils * num
        // timesteps. Thus, the number of overlapping regions is ceil(total
        // shift / region size).  This assumes stencils are inter-dependent.
        // TODO: calculate stencil inter-dependency in the foldBuilder for each
        // dimension.
        idx_t nshifts = (idx_t(eqGroups.size()) * _opts->rt) - 1;
        end_dn += angle_n * nshifts;
        end_dx += angle_x * nshifts;
        end_dy += angle_y * nshifts;
        end_dz += angle_z * nshifts;
        TRACE_MSG("extended domain after wave-front adjustment:"
                  " %ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld", 
                  begin_dt, end_dt-1,
                  begin_dn, end_dn-1,
                  begin_dx, end_dx-1,
                  begin_dy, end_dy-1,
                  begin_dz, end_dz-1);

        // Number of iterations to get from begin_dt to (but not including) end_dt,
        // stepping by step_dt.
        const idx_t num_dt = ((end_dt - begin_dt) + (step_dt - 1)) / step_dt;
        for (idx_t index_dt = 0; index_dt < num_dt; index_dt++)
        {
            // This value of index_dt covers dt from start_dt to stop_dt-1.
            const idx_t start_dt = begin_dt + (index_dt * step_dt);
            const idx_t stop_dt = min(start_dt + step_dt, end_dt);

            // FIXME: halo exchange with conditional equations is broken.
            
            // If doing only one time step in a region (default), loop through equations here,
            // and do only one equation group at a time in calc_region().
            if (step_dt == 1) {

                for (auto eqGroup : eqGroups) {

                    // Halo exchange for grid(s) updated by this equation.
                    eqGroup->exchange_halos(start_dt, stop_dt);

                    // Eval this stencil in calc_region().
                    EqGroupSet eqGroup_set;
                    eqGroup_set.insert(eqGroup);
                    EqGroupSet* eqGroup_ptr = &eqGroup_set;

                    // Include automatically-generated loop code that calls calc_region() for each region.
#include "stencil_rank_loops.hpp"
                }
            }

            // If doing more than one time step in a region (temporal wave-front),
            // must do all equations in calc_region().
            else {

                // Eval all equation groups.
                EqGroupSet* eqGroup_ptr = NULL;
                
                for (auto eqGroup : eqGroups) {

                    // Halo exchange for grid(s) updated by this equation.
                    eqGroup->exchange_halos(start_dt, stop_dt);
                }
            
                // Include automatically-generated loop code that calls calc_region() for each region.
#include "stencil_rank_loops.hpp"
            }

        }

#ifdef MODEL_CACHE
        // Print cache stats, then disable.
        // Thus, cache is only modeled for first call.
        if (cache_model.isEnabled()) {
            os << "Done modeling cache...\n";
            cache_model.dumpStats();
            cache_model.disable();
        }
#endif

    }

    // Calculate results within a region.
    // Each region is typically computed in a separate OpenMP 'for' region.
    // In it, we loop over the time steps and the stencil
    // equations and evaluate the blocks in the region.
    void StencilContext::calc_region(idx_t start_dt, idx_t stop_dt,
                                     EqGroupSet* eqGroup_set,
                                     idx_t start_dn, idx_t start_dx, idx_t start_dy, idx_t start_dz,
                                     idx_t stop_dn, idx_t stop_dx, idx_t stop_dy, idx_t stop_dz)
    {
        TRACE_MSG("calc_region(%ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld, %ld..%ld)", 
                  start_dt, stop_dt-1,
                  start_dn, stop_dn-1,
                  start_dx, stop_dx-1,
                  start_dy, stop_dy-1,
                  start_dz, stop_dz-1);

        // Steps within a region are based on block sizes.
        const idx_t step_rt = _opts->bt;
        const idx_t step_rn = _opts->bn;
        const idx_t step_rx = _opts->bx;
        const idx_t step_ry = _opts->by;
        const idx_t step_rz = _opts->bz;

        // Groups in region loops are based on group sizes.
        const idx_t group_size_rn = _opts->gn;
        const idx_t group_size_rx = _opts->gx;
        const idx_t group_size_ry = _opts->gy;
        const idx_t group_size_rz = _opts->gz;

        // Not yet supporting temporal blocking.
        if (step_rt != 1) {
            cerr << "Error: temporal blocking not yet supported." << endl;
            assert(step_rt == 1);
            exit_yask(1);                // in case assert() is not active.
        }

        // Number of iterations to get from start_dt to (but not including) stop_dt,
        // stepping by step_rt.
        const idx_t num_rt = ((stop_dt - start_dt) + (step_rt - 1)) / step_rt;
    
        // Step through time steps in this region.
        for (idx_t index_rt = 0; index_rt < num_rt; index_rt++) {
        
            // This value of index_rt covers rt from start_rt to stop_rt-1.
            const idx_t start_rt = start_dt + (index_rt * step_rt);
            const idx_t stop_rt = min (start_rt + step_rt, stop_dt);

            // TODO: remove this when temporal blocking is implemented.
            assert(stop_rt == start_rt + 1);
            const idx_t rt = start_rt; // only one time value needed for block.

            // equations to evaluate at this time step.
            for (auto eg : eqGroups) {
                if (!eqGroup_set || eqGroup_set->count(eg)) {

                    // Actual region boundaries must stay within BB for this eq group.
                    idx_t begin_rn = max<idx_t>(start_dn, eg->begin_bbn);
                    idx_t end_rn = min<idx_t>(stop_dn, eg->end_bbn);
                    idx_t begin_rx = max<idx_t>(start_dx, eg->begin_bbx);
                    idx_t end_rx = min<idx_t>(stop_dx, eg->end_bbx);
                    idx_t begin_ry = max<idx_t>(start_dy, eg->begin_bby);
                    idx_t end_ry = min<idx_t>(stop_dy, eg->end_bby);
                    idx_t begin_rz = max<idx_t>(start_dz, eg->begin_bbz);
                    idx_t end_rz = min<idx_t>(stop_dz, eg->end_bbz);

                    // Only need to loop through the region if any of its blocks are
                    // at least partly inside the domain. For overlapping regions,
                    // they may start outside the domain but enter the domain as
                    // time progresses and their boundaries shift. So, we don't want
                    // to return if this condition isn't met.
                    if (end_rn > begin_rn &&
                        end_rx > begin_rx &&
                        end_ry > begin_ry &&
                        end_rz > begin_rz) {

                        // Set number of threads for a region.
                        set_region_threads();

                        // Include automatically-generated loop code that calls
                        // calc_block() for each block in this region.  Loops
                        // through n from begin_rn to end_rn-1; similar for x, y,
                        // and z.  This code typically contains OpenMP loop(s).
#include "stencil_region_loops.hpp"

                        // Reset threads back to max.
                        set_all_threads();
                    }
            
                    // Shift spatial region boundaries for next iteration to
                    // implement temporal wavefront.  We only shift backward, so
                    // region loops must increment. They may do so in any order.
                    start_dn -= angle_n;
                    stop_dn -= angle_n;
                    start_dx -= angle_x;
                    stop_dx -= angle_x;
                    start_dy -= angle_y;
                    stop_dy -= angle_y;
                    start_dz -= angle_z;
                    stop_dz -= angle_z;

                }            
            } // stencil equations.
        } // time.
    }

    // Init MPI-related vars and other vars related to my rank's place in
    // the global problem.  Need to call this even if not using MPI to
    // properly init some vars.  Called from allocAll(), so it doesn't
    // normally need to be called from user code.
    void StencilContext::setupRank() {
        ostream& os = get_ostr();

        // Report ranks.
        os << "Num ranks: " << num_ranks << endl;
        os << "This rank index: " << my_rank << endl;

        // Check ranks.
        idx_t req_ranks = _opts->nrn * _opts->nrx * _opts->nry * _opts->nrz;
        if (req_ranks != num_ranks) {
            cerr << "error: " << req_ranks << " rank(s) requested, but " <<
                num_ranks << " rank(s) are active." << endl;
            exit_yask(1);
        }
        assertEqualityOverRanks(_opts->dt, comm, "time-step");

        // Determine my coordinates if not provided already.
        // TODO: do this more intelligently based on proximity.
        if (_opts->find_loc) {
            Layout_4321 rank_layout(_opts->nrn, _opts->nrx, _opts->nry, _opts->nrz);
            rank_layout.unlayout((idx_t)my_rank,
                                 _opts->rin, _opts->rix, _opts->riy, _opts->riz);
        }
        os << "Logical coordinates of this rank: " <<
            _opts->rin << ", " <<
            _opts->rix << ", " <<
            _opts->riy << ", " <<
            _opts->riz << endl;

        // A table of rank-coordinates for everyone.
        const int num_dims = 4;
        idx_t coords[num_ranks][num_dims];

        // Init coords for this rank.
        coords[my_rank][0] = _opts->rin;
        coords[my_rank][1] = _opts->rix;
        coords[my_rank][2] = _opts->riy;
        coords[my_rank][3] = _opts->riz;

        // A table of rank-sizes for everyone.
        idx_t rsizes[num_ranks][num_dims];

        // Init sizes for this rank.
        rsizes[my_rank][0] = _opts->dn;
        rsizes[my_rank][1] = _opts->dx;
        rsizes[my_rank][2] = _opts->dy;
        rsizes[my_rank][3] = _opts->dz;

#ifdef USE_MPI
        // Exchange coord and size info between all ranks.
        for (int rn = 0; rn < num_ranks; rn++) {
            MPI_Bcast(&coords[rn][0], num_dims, MPI_INTEGER8,
                      rn, comm);
            MPI_Bcast(&rsizes[rn][0], num_dims, MPI_INTEGER8,
                      rn, comm);
        }
#endif
        
        ofs_n = ofs_x = ofs_y = ofs_z = 0;
        tot_n = tot_x = tot_y = tot_z = 0;
        int num_neighbors = 0, num_exchanges = 0;
        for (int rn = 0; rn < num_ranks; rn++) {

            // Get coordinates of rn.
            idx_t rnn = coords[rn][0];
            idx_t rnx = coords[rn][1];
            idx_t rny = coords[rn][2];
            idx_t rnz = coords[rn][3];

            // Coord offset of rn from me: prev => negative, self => 0, next => positive.
            idx_t rdn = rnn - _opts->rin;
            idx_t rdx = rnx - _opts->rix;
            idx_t rdy = rny - _opts->riy;
            idx_t rdz = rnz - _opts->riz;

            // Get sizes of rn;
            idx_t rsn = rsizes[rn][0];
            idx_t rsx = rsizes[rn][1];
            idx_t rsy = rsizes[rn][2];
            idx_t rsz = rsizes[rn][3];
        
            // Accumulate total problem size in each dim for ranks that
            // intersect with this rank, including myself.
            // Adjust my offset in the global problem by adding all domain
            // sizes from prev ranks only.
            if (rdx == 0 && rdy == 0 && rdz == 0) {
                tot_n += rsn;
                if (rdn < 0)
                    ofs_n += rsn;
            }
            if (rdn == 0 && rdy == 0 && rdz == 0) {
                tot_x += rsx;
                if (rdx < 0)
                    ofs_x += rsx;
            }
            if (rdn == 0 && rdx == 0 && rdz == 0) {
                tot_y += rsy;
                if (rdy < 0)
                    ofs_y += rsy;
            }
            if (rdn == 0 && rdx == 0 && rdy == 0) {
                tot_z += rsz;
                if (rdz < 0)
                    ofs_z += rsz;
            }
            
            // Manhattan distance.
            int mdist = abs(rdn) + abs(rdx) + abs(rdy) + abs(rdz);
            
            // Myself.
            if (rn == my_rank) {
                if (mdist != 0) {
                    cerr << "internal error: distance to own rank == " << mdist << endl;
                    exit_yask(1);
                }
                continue; // nothing else to do for self.
            }

            // Someone else.
            else {
                if (mdist == 0) {
                    cerr << "error: ranks " << my_rank <<
                        " and " << rn << " at same coordinates." << endl;
                    exit_yask(1);
                }
            }
            
            // Rank rn is my immediate neighbor if its distance <= 1 in
            // every dim.  Assume we do not need to exchange halos except
            // with immediate neighbor. TODO: validate domain size is larger
            // than halo.
            if (abs(rdn) > 1 || abs(rdx) > 1 || abs(rdy) > 1 || abs(rdz) > 1)
                continue;

            // Size of buffer in each direction: if dist to neighbor is zero
            // (i.e., is perpendicular to this rank), use full size;
            // otherwise, use halo size.  TODO: use per-grid actual halo
            // size determined by stencil compiler instead of global max.
            idx_t bsn = (rdn == 0) ? _opts->dn : hn;
            idx_t bsx = (rdx == 0) ? _opts->dx : hx;
            idx_t bsy = (rdy == 0) ? _opts->dy : hy;
            idx_t bsz = (rdz == 0) ? _opts->dz : hz;

            // Add one to -1..+1 dist to get 0..2 range for my_neighbors indices.
            rdn++; rdx++; rdy++; rdz++;

            // Save rank of this neighbor.
            my_neighbors[rdn][rdx][rdy][rdz] = rn;
            num_neighbors++;
            os << "Neighbor #" << num_neighbors << " at " <<
                rnn << ", " << rnx << ", " << rny << ", " << rnz <<
                " is rank " << rn << endl;
                    
            // Check against max dist needed.  TODO: determine max dist
            // automatically from stencil equations; may not be same for all
            // grids.
#ifndef MAX_EXCH_DIST
#define MAX_EXCH_DIST 4
#endif

            // Is buffer needed?
            if (mdist > MAX_EXCH_DIST || bsn * bsx * bsy * bsz == 0) {
                os << " No halo exchange with rank " << rn << '.' << endl;
            }
            else {
                
                // Alloc MPI buffers between rn and my rank.
                // Need send and receive for each updated grid.
                for (auto gname : outputGridNames) {
                    for (int bd = 0; bd < MPIBufs::nBufDirs; bd++) {
                        ostringstream oss;
                        oss << gname;
                        if (bd == MPIBufs::bufSend)
                            oss << "_send_halo_from_" << my_rank << "_to_" << rn;
                        else
                            oss << "_get_halo_to_" << my_rank << "_from_" << rn;
                        
                        mpiBufs[gname].allocBuf(bd,
                                                rdn, rdx, rdy, rdz,
                                                bsn, bsx, bsy, bsz,
                                                oss.str(), os);
                        num_exchanges++;
                    }
                }
            }
        }
        os << "Problem-domain offsets of this rank: " <<
            ofs_n << ", " << ofs_x << ", " << ofs_y << ", " << ofs_z << endl;
        os << "Number of halo exchanges from this rank: " << num_exchanges << endl;
    }

    // Get total size.
    idx_t StencilContext::get_num_bytes() {
        idx_t nbytes = 0;

        // Grids.
        for (auto gp : gridPtrs)
            nbytes += gp->get_num_bytes();

        // Params.
        for (auto pp : paramPtrs)
            nbytes += pp->get_num_bytes();

        // MPI buffers.
        for (auto gname : outputGridNames) {
            mpiBufs[gname].visitNeighbors
                (*this,
                 [&](idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                     int rank,
                     Grid_NXYZ* sendBuf,
                     Grid_NXYZ* rcvBuf)
                 {
                     if (sendBuf)
                         nbytes += sendBuf->get_num_bytes();
                     if (rcvBuf)
                         nbytes += rcvBuf->get_num_bytes();
                 } );
        }

        return nbytes;
    }

    // Allocate grids, params, and MPI bufs.
    // Initialize some data structures.
    void StencilContext::allocAll()
    {
        // Don't continue until all ranks are this far.
        global_barrier();

        ostream& os = get_ostr();
#ifdef DEBUG
        os << "*** WARNING: YASK compiled with DEBUG; ignore performance results.\n";
#endif
#if defined(NO_INTRINSICS) && (VLEN > 1)
        os << "*** WARNING: YASK compiled with NO_INTRINSICS; ignore performance results.\n";
#endif
#ifdef MODEL_CACHE
        os << "*** WARNING: YASK compiled with MODEL_CACHE; ignore performance results.\n";
#endif
#ifdef TRACE_MEM
        os << "*** WARNING: YASK compiled with TRACE_MEM; ignore performance results.\n";
#endif
#ifdef TRACE_INTRINSICS
        os << "*** WARNING: YASK compiled with TRACE_INTRINSICS; ignore performance results.\n";
#endif
        
        // Adjust all settings needed before allocating data.
        _opts->finalizeSettings(os);
        
        // report threads.
        os << endl;
        os << "Num OpenMP procs: " << omp_get_num_procs() << endl;
        set_all_threads();
        os << "Num OpenMP threads: " << omp_get_max_threads() << endl;
        set_region_threads(); // Temporary; just for reporting.
        os << "  Num threads per region: " << omp_get_max_threads() << endl;
        set_block_threads(); // Temporary; just for reporting.
        os << "  Num threads per block: " << omp_get_max_threads() << endl;
        set_all_threads(); // Back to normal.

        // TODO: enable multi-rank wave-front tiling.
        if (_opts->rt > 1 && num_ranks > 1) {
            cerr << "MPI communication is not currently enabled with wave-front tiling." << endl;
            exit_yask(1);
        }

        // TODO: check all dims.
#ifndef USING_DIM_N
        if (_opts->dn > 1) {
            cerr << "error: dn = " << _opts->dn << ", but stencil '"
                YASK_STENCIL_NAME "' doesn't use dimension 'n'." << endl;
            exit_yask(1);
        }
#endif

        os << endl;
        os << "Num grids: " << gridNames.size() << endl;
        os << "Num grids to be updated: " << outputGridNames.size() << endl;
        os << "Num stencil equation-groups: " << eqGroups.size() << endl;
        
        // Set up MPI data.  Must do this before allocating grids so that
        // global offsets are calculated properly.
        if (num_ranks > 1)
            os << "Allocating MPI buffers..." << endl;
        setupRank();

        // Alloc grids and params.
        os << "Allocating grids..." << endl;
        allocGrids();
        os << "Allocating parameters..." << endl;
        allocParams();

        // Set pointers to the allocated data.
        setPtrs();
        for (auto eg : eqGroups)
            eg->setPtrs(*this);
        find_bounding_boxes();

        // Report some stats.
        idx_t dt = _opts->dt;
        os << "\nSizes in points per grid (t*n*x*y*z):\n"
            " vector-size:      " << VLEN_T << '*' << VLEN_N << '*' << VLEN_X << '*' << VLEN_Y << '*' << VLEN_Z << endl <<
            " cluster-size:     " << CPTS_T << '*' << CPTS_N << '*' << CPTS_X << '*' << CPTS_Y << '*' << CPTS_Z << endl <<
            " block-size:       " << _opts->bt << '*' << _opts->bn << '*' << _opts->bx << '*' << _opts->by << '*' << _opts->bz << endl <<
            " block-group-size: 1*" << _opts->gn << '*' << _opts->gx << '*' << _opts->gy << '*' << _opts->gz << endl <<
            " region-size:      " << _opts->rt << '*' << _opts->rn << '*' << _opts->rx << '*' << _opts->ry << '*' << _opts->rz << endl <<
            " rank-domain-size: " << dt << '*' << _opts->dn << '*' << _opts->dx << '*' << _opts->dy << '*' << _opts->dz << endl <<
            " problem-size:     " << dt << '*' << tot_n << '*' << tot_x << '*' << tot_y << '*' << tot_z << endl <<
            endl <<
            "Other settings:\n"
            " num-ranks: " << _opts->nrn << '*' << _opts->nrx << '*' << _opts->nry << '*' << _opts->nrz << endl <<
            " stencil-name: " YASK_STENCIL_NAME << endl << 
            " time-dim-size: " << TIME_DIM_SIZE << endl <<
            " vector-len: " << VLEN << endl <<
            " padding: " << _opts->pn << '+' << _opts->px << '+' << _opts->py << '+' << _opts->pz << endl <<
            " wave-front-angles: " << angle_n << '+' << angle_x << '+' << angle_y << '+' << angle_z << endl <<
            " max-halos: " << hn << '+' << hx << '+' << hy << '+' << hz << endl <<
            " manual-L1-prefetch-distance: " << PFDL1 << endl <<
            " manual-L2-prefetch-distance: " << PFDL2 << endl <<
            endl;
        
        rank_numpts_1t = 0; rank_numFpOps_1t = 0; // sums across eqs for this rank.
        for (auto eg : eqGroups) {
            idx_t updates1 = eg->get_scalar_points_updated();
            idx_t updates_domain = updates1 * eg->bb_size;
            idx_t fpops1 = eg->get_scalar_fp_ops();
            idx_t fpops_domain = fpops1 * eg->bb_size;
            rank_numpts_1t += updates_domain;
            rank_numFpOps_1t += fpops_domain;
            os << "Stats for equation-group '" << eg->get_name() << "':\n" <<
                " sub-domain-size:            " <<
                eg->len_bbn << '*' << eg->len_bbx << '*' << eg->len_bby << '*' << eg->len_bbz << endl <<
                " points-in-sub-domain:       " << printWithPow10Multiplier(eg->bb_size) << endl <<
                " grid-updates-per-point:     " << updates1 << endl <<
                " grid-updates-in-sub-domain: " << printWithPow10Multiplier(updates_domain) << endl <<
                " est-FP-ops-per-point:       " << fpops1 << endl <<
                " est-FP-ops-in-sub-domain:   " << printWithPow10Multiplier(fpops_domain) << endl;
        }

        // Report total allocation.
        rank_nbytes = get_num_bytes();
        os << "Total allocation in this rank (bytes): " <<
            printWithPow2Multiplier(rank_nbytes) << endl;
        tot_nbytes = sumOverRanks(rank_nbytes, comm);
        os << "Total overall allocation in " << num_ranks << " rank(s) (bytes): " <<
            printWithPow2Multiplier(tot_nbytes) << endl;
    
        // Various metrics for amount of work.
        rank_numpts_dt = rank_numpts_1t * dt;
        tot_numpts_1t = sumOverRanks(rank_numpts_1t, comm);
        tot_numpts_dt = tot_numpts_1t * dt;

        rank_numFpOps_dt = rank_numFpOps_1t * dt;
        tot_numFpOps_1t = sumOverRanks(rank_numFpOps_1t, comm);
        tot_numFpOps_dt = tot_numFpOps_1t * dt;

        rank_domain_1t = _opts->dn * _opts->dx * _opts->dy * _opts->dz;
        rank_domain_dt = rank_domain_1t * dt;
        tot_domain_1t = sumOverRanks(rank_domain_1t, comm);
        tot_domain_dt = tot_domain_1t * dt;
    
        // Print some more stats.
        os << endl <<
            "Amount-of-work stats:\n" <<
            " problem-size in this rank, for one time-step: " <<
            printWithPow10Multiplier(rank_domain_1t) << endl <<
            " problem-size in all ranks, for one time-step: " <<
            printWithPow10Multiplier(tot_domain_1t) << endl <<
            " problem-size in this rank, for all time-steps: " <<
            printWithPow10Multiplier(rank_domain_dt) << endl <<
            " problem-size in all ranks, for all time-steps: " <<
            printWithPow10Multiplier(tot_domain_dt) << endl <<
            endl <<
            " grid-points-updated in this rank, for one time-step: " <<
            printWithPow10Multiplier(rank_numpts_1t) << endl <<
            " grid-points-updated in all ranks, for one time-step: " <<
            printWithPow10Multiplier(tot_numpts_1t) << endl <<
            " grid-points-updated in this rank, for all time-steps: " <<
            printWithPow10Multiplier(rank_numpts_dt) << endl <<
            " grid-points-updated in all ranks, for all time-steps: " <<
            printWithPow10Multiplier(tot_numpts_dt) << endl <<
            endl <<
            " est-FP-ops in this rank, for one time-step: " <<
            printWithPow10Multiplier(rank_numFpOps_1t) << endl <<
            " est-FP-ops in all ranks, for one time-step: " <<
            printWithPow10Multiplier(tot_numFpOps_1t) << endl <<
            " est-FP-ops in this rank, for all time-steps: " <<
            printWithPow10Multiplier(rank_numFpOps_dt) << endl <<
            " est-FP-ops in all ranks, for all time-steps: " <<
            printWithPow10Multiplier(tot_numFpOps_dt) << endl <<
            endl << 
            "Notes:\n" <<
            " problem-size is based on rank-domain sizes specified in command-line (dn * dx * dy * dz).\n" <<
            " grid-points-updated is based sum of grid-updates-in-sub-domain across equation-group(s).\n" <<
            " est-FP-ops is based on sum of est-FP-ops-in-sub-domain across equation-group(s).\n" <<
            endl;

    }

    // Init all grids & params by calling initFn.
    void StencilContext::initValues(function<void (RealVecGridBase* gp, 
                                                   real_t seed)> realVecInitFn,
                                    function<void (RealGrid* gp,
                                                   real_t seed)> realInitFn)
    {
        ostream& os = get_ostr();
        real_t v = 0.1;
        os << "Initializing grids..." << endl;
        for (auto gp : gridPtrs) {
            realVecInitFn(gp, v);
            v += 0.01;
        }
        if (paramPtrs.size()) {
            os << "Initializing parameters..." << endl;
            for (auto pp : paramPtrs) {
                realInitFn(pp, v);
                v += 0.01;
            }
        }
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
            os << "Grid '" << ref.gridPtrs[gi]->get_name() << "'..." << endl;
            errs += gridPtrs[gi]->compare(*ref.gridPtrs[gi]);
        }

        os << "Comparing parameter(s) in '" << name << "' to '" << ref.name << "'..." << endl;
        if (paramPtrs.size() != ref.paramPtrs.size()) {
            cerr << "** number of params not equal." << endl;
            return 1;
        }
        for (size_t pi = 0; pi < paramPtrs.size(); pi++) {
            errs += paramPtrs[pi]->compare(ref.paramPtrs[pi], EPSILON);
        }

        return errs;
    }
    
    
    // Set the bounding-box around all eq groups.
    void StencilContext::find_bounding_boxes()
    {
        if (bb_valid == true) return;

        // Init overall BB.
        // Init min vars w/max val and vice-versa.
        begin_bbn = idx_max; end_bbn = idx_min;
        begin_bbx = idx_max; end_bbx = idx_min;
        begin_bby = idx_max; end_bby = idx_min;
        begin_bbz = idx_max; end_bbz = idx_min;
        bb_size = 0;
        
        // Find BB for each eq group and update context.
        for (auto eg : eqGroups) {
            eg->find_bounding_box();

            begin_bbn = min(begin_bbn, eg->begin_bbn);
            begin_bbx = min(begin_bbx, eg->begin_bbx);
            begin_bby = min(begin_bby, eg->begin_bby);
            begin_bbz = min(begin_bbz, eg->begin_bbz);
            end_bbn = max(end_bbn, eg->end_bbn);
            end_bbx = max(end_bbx, eg->end_bbx);
            end_bby = max(end_bby, eg->end_bby);
            end_bbz = max(end_bbz, eg->end_bbz);
            bb_size += eg->bb_size;
        }

        len_bbn = end_bbn - begin_bbn;
        len_bbx = end_bbx - begin_bbx;
        len_bby = end_bby - begin_bby;
        len_bbz = end_bbz - begin_bbz;
        bb_valid = true;

        // Adjust region size to be within BB.
        _opts->rn = min(_opts->rn, len_bbn);
        _opts->rx = min(_opts->rx, len_bbx);
        _opts->ry = min(_opts->ry, len_bby);
        _opts->rz = min(_opts->rz, len_bbz);

        // Adjust block size to be within region.
        _opts->bn = min(_opts->bn, _opts->rn);
        _opts->bx = min(_opts->bx, _opts->rx);
        _opts->by = min(_opts->by, _opts->ry);
        _opts->bz = min(_opts->bz, _opts->rz);

        // Determine spatial skewing angles for temporal wavefronts based on
        // the halos.  This assumes the smallest granularity of calculation
        // is CPTS_* in each dim.  We only need non-zero angles if the
        // region size is less than the rank size, i.e., if the region
        // covers the whole rank in a given dimension, no wave-front is
        // needed in thar dim.  TODO: make this grid-specific.
        angle_n = (_opts->rn < len_bbn) ? ROUND_UP(hn, CPTS_N) : 0;
        angle_x = (_opts->rx < len_bbx) ? ROUND_UP(hx, CPTS_X) : 0;
        angle_y = (_opts->ry < len_bby) ? ROUND_UP(hy, CPTS_Y) : 0;
        angle_z = (_opts->rz < len_bbz) ? ROUND_UP(hz, CPTS_Z) : 0;
    }

    // Set the bounding-box vars for this eq group in this rank.
    void EqGroupBase::find_bounding_box() {
        if (bb_valid) return;
        StencilContext& context = *_generic_context;
        StencilSettings& opts = context.get_settings();

        // Init min vars w/max val and vice-versa.
        idx_t minn = idx_max, maxn = idx_min;
        idx_t minx = idx_max, maxx = idx_min;
        idx_t miny = idx_max, maxy = idx_min;
        idx_t minz = idx_max, maxz = idx_min;
        idx_t npts = 0;
        
        // Assume bounding-box is same for all time steps.
        // TODO: consider adding time to domain.
        idx_t t = 0;

        // Loop through 4D space.
        // Find the min and max valid points in this space.
#pragma omp parallel for collapse(4)            \
    reduction(min:minn,minx,miny,minz)          \
    reduction(max:maxn,maxx,maxy,maxz)          \
    reduction(+:npts)
        for (idx_t n = context.ofs_n; n < context.ofs_n + opts.dn; n++)
            for(idx_t x = context.ofs_x; x < context.ofs_x + opts.dx; x++)
                for(idx_t y = context.ofs_y; y < context.ofs_y + opts.dy; y++)
                    for(idx_t z = context.ofs_z; z < context.ofs_z + opts.dz; z++) {

                        // Update only if point in domain for this eq group.
                        if (is_in_valid_domain(t, n, x, y, z)) {
                            minn = min(minn, n);
                            maxn = max(maxn, n);
                            minx = min(minx, x);
                            maxx = max(maxx, x);
                            miny = min(miny, y);
                            maxy = max(maxy, y);
                            minz = min(minz, z);
                            maxz = max(maxz, z);
                            npts++;
                        }
                    }

        // Set begin vars to min indices and end vars to one beyond max indices.
        if (npts) {
            begin_bbn = minn;
            end_bbn = maxn + 1;
            begin_bbx = minx;
            end_bbx = maxx + 1;
            begin_bby = miny;
            end_bby = maxy + 1;
            begin_bbz = minz;
            end_bbz = maxz + 1;
        } else {
            begin_bbn = end_bbn = len_bbn = 0;
            begin_bbx = end_bbx = len_bbx = 0;
            begin_bby = end_bby = len_bby = 0;
            begin_bbz = end_bbz = len_bbz = 0;
        }
        len_bbn = end_bbn - begin_bbn;
        len_bbx = end_bbx - begin_bbx;
        len_bby = end_bby - begin_bby;
        len_bbz = end_bbz - begin_bbz;
        bb_size = npts;

        // Only supporting solid rectangles at this time.
        idx_t r_size = len_bbn * len_bbx * len_bby * len_bbz;
        if (r_size != bb_size) {
            cerr << "error: domain for equation-group '" << get_name() << "' contains " <<
                bb_size << " points, but " << r_size << " were expected for a rectangular solid. " <<
                "Non-rectangular domains are not supported at this time." << endl;
            exit_yask(1);
        }

        // Only supporting full-cluster BBs at this time.
        // TODO: handle partial clusters.
        if (len_bbn % CLEN_N ||
            len_bbx % CLEN_X ||
            len_bby % CLEN_Y ||
            len_bbz % CLEN_Z) {
            cerr << "error: each domain length must be a multiple of the cluster size." << endl;
            exit_yask(1);
        }

        bb_valid = true;
    }
    
    // Exchange halo data for the given time.
    void EqGroupBase::exchange_halos(idx_t start_dt, idx_t stop_dt)
    {
        StencilContext& context = *_generic_context;
        StencilSettings& opts = context.get_settings();
        TRACE_MSG("exchange_halos(%ld..%ld)", start_dt, stop_dt);

        // List of grids updated by this equation.
        // These are the grids that need exchanges.
        // FIXME: does not work w/conditions.
        auto outputGridPtrs = context.outputGridPtrs;

#ifdef USE_MPI
        double start_time = getTimeInSecs();

        // These vars control blocking within halo packing.
        // Currently, only zv has a loop in the calc_halo macros below.
        // Thus, step_{n,x,y}v must be 1.
        // TODO: make step_zv a parameter.
        const idx_t step_nv = 1;
        const idx_t step_xv = 1;
        const idx_t step_yv = 1;
        const idx_t step_zv = 4;

        // Groups in halo loops are set to smallest size.
        const idx_t group_size_nv = 1;
        const idx_t group_size_xv = 1;
        const idx_t group_size_yv = 1;
        const idx_t group_size_zv = 1;

        // TODO: put this loop inside visitNeighbors.
        for (size_t gi = 0; gi < outputGridPtrs.size(); gi++) {

            // Get pointer to generic grid and derived type.
            // TODO: Make this more general.
            auto gp = outputGridPtrs[gi];
#if USING_DIM_N
            auto gpd = dynamic_cast<Grid_TNXYZ*>(gp);
#else
            auto gpd = dynamic_cast<Grid_TXYZ*>(gp);
#endif
            assert(gpd);
            auto gname = gp->get_name();

            // Determine halo sizes to be exchanged for this grid;
            // context.h* contains the max value across all grids.  The grid
            // contains the halo+pad size actually allocated.
            // Since neither of these is exactly what we want, we use
            // the minimum of these values as a conservative value. TODO:
            // Store the actual halo needed in each grid and use this.
#if USING_DIM_N
            idx_t hn = min(context.hn, gpd->get_pn());
#else
            idx_t hn = 0;
#endif
            idx_t hx = min(context.hx, gpd->get_px());
            idx_t hy = min(context.hy, gpd->get_py());
            idx_t hz = min(context.hz, gpd->get_pz());
            
            // Array to store max number of request handles.
            MPI_Request reqs[MPIBufs::nBufDirs * MPIBufs::neighborhood_size];
            int nreqs = 0;

            // Pack data and initiate non-blocking send/receive to/from all neighbors.
            TRACE_MSG("rank %i: exchange_halos: packing data for grid '%s'...",
                      context.my_rank, gname.c_str());
            assert(context.mpiBufs.count(gname) != 0);
            context.mpiBufs[gname].visitNeighbors
                (context,
                 [&](idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                     int neighbor_rank,
                     Grid_NXYZ* sendBuf,
                     Grid_NXYZ* rcvBuf)
                 {
                     // Pack and send data if buffer exists.
                     if (sendBuf) {

                         // Set begin/end vars to indicate what part
                         // of main grid to read from.
                         // Init range to whole rank domain (inside halos).
                         idx_t begin_n = 0;
                         idx_t begin_x = 0;
                         idx_t begin_y = 0;
                         idx_t begin_z = 0;
                         idx_t end_n = opts.dn;
                         idx_t end_x = opts.dx;
                         idx_t end_y = opts.dy;
                         idx_t end_z = opts.dz;

                         // Modify begin and/or end based on direction.
                         if (nn == idx_t(MPIBufs::rank_prev)) // neighbor is prev N.
                             end_n = hn; // read first halo-width only.
                         if (nn == idx_t(MPIBufs::rank_next)) // neighbor is next N.
                             begin_n = opts.dn - hn; // read last halo-width only.
                         if (nx == idx_t(MPIBufs::rank_prev)) // neighbor is on left.
                             end_x = hx;
                         if (nx == idx_t(MPIBufs::rank_next)) // neighbor is on right.
                             begin_x = opts.dx - hx;
                         if (ny == idx_t(MPIBufs::rank_prev)) // neighbor is in front.
                             end_y = hy;
                         if (ny == idx_t(MPIBufs::rank_next)) // neighbor is in back.
                             begin_y = opts.dy - hy;
                         if (nz == idx_t(MPIBufs::rank_prev)) // neighbor is above.
                             end_z = hz;
                         if (nz == idx_t(MPIBufs::rank_next)) // neighbor is below.
                             begin_z = opts.dz - hz;

                         // Add offsets and divide indices by vector
                         // lengths.  Begin/end vars shouldn't be negative
                         // (because we're always inside the halo), so '/'
                         // is ok.
                         idx_t begin_nv = (context.ofs_n + begin_n) / VLEN_N;
                         idx_t begin_xv = (context.ofs_x + begin_x) / VLEN_X;
                         idx_t begin_yv = (context.ofs_y + begin_y) / VLEN_Y;
                         idx_t begin_zv = (context.ofs_z + begin_z) / VLEN_Z;
                         idx_t end_nv = (context.ofs_n + end_n) / VLEN_N;
                         idx_t end_xv = (context.ofs_x + end_x) / VLEN_X;
                         idx_t end_yv = (context.ofs_y + end_y) / VLEN_Y;
                         idx_t end_zv = (context.ofs_z + end_z) / VLEN_Z;

                         // TODO: fix this when MPI + wave-front is enabled.
                         idx_t t = start_dt;
                         
                         // Define calc_halo() to copy a vector from main grid to sendBuf.
                         // Index sendBuf using index_* vars because they are zero-based.
#define calc_halo(t,                                                    \
                  start_nv, start_xv, start_yv, start_zv,               \
                  stop_nv, stop_xv, stop_yv, stop_zv)  do {             \
                         idx_t nv = start_nv;                           \
                         idx_t xv = start_xv;                           \
                         idx_t yv = start_yv;                           \
                         idx_t izv = index_zv * step_zv;                \
                         for (idx_t zv = start_zv; zv < stop_zv; zv++) { \
                             real_vec_t hval = gpd->readVecNorm(t, ARG_N(nv) \
                                                                xv, yv, zv, __LINE__); \
                             sendBuf->writeVecNorm(hval, index_nv,      \
                                                   index_xv, index_yv, izv++, __LINE__); \
                         } } while(0)
                         
                         // Include auto-generated loops to invoke calc_halo() from
                         // begin_*v to end_*v;
#include "stencil_halo_loops.hpp"
#undef calc_halo

                         // Send filled buffer to neighbor.
                         const void* buf = (const void*)(sendBuf->getRawData());
                         MPI_Isend(buf, sendBuf->get_num_bytes(), MPI_BYTE,
                                   neighbor_rank, int(gi), context.comm, &reqs[nreqs++]);
                     }

                     // Receive data from same neighbor if buffer exists.
                     if (rcvBuf) {
                         void* buf = (void*)(rcvBuf->getRawData());
                         MPI_Irecv(buf, rcvBuf->get_num_bytes(), MPI_BYTE,
                                   neighbor_rank, int(gi), context.comm, &reqs[nreqs++]);
                     }
                     
                 } );

            // Wait for all to complete.
            // TODO: process each buffer asynchronously immediately upon completion.
            TRACE_MSG("rank %i: exchange_halos: waiting for %i MPI request(s)...",
                      context.my_rank, nreqs);
            MPI_Waitall(nreqs, reqs, MPI_STATUS_IGNORE);
            TRACE_MSG("rank %i: exchange_halos: done waiting for %i MPI request(s).",
                      context.my_rank, nreqs);

            // Unpack received data from all neighbors.
            assert(context.mpiBufs.count(gname) != 0);
            context.mpiBufs[gname].visitNeighbors
                (context,
                 [&](idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                     int neighbor_rank,
                     Grid_NXYZ* sendBuf,
                     Grid_NXYZ* rcvBuf)
                 {
                     // Unpack data if buffer exists.
                     if (rcvBuf) {

                         // Set begin/end vars to indicate what part
                         // of main grid's halo to write to.
                         // Init range to whole rank size (inside halos).
                         idx_t begin_n = 0;
                         idx_t begin_x = 0;
                         idx_t begin_y = 0;
                         idx_t begin_z = 0;
                         idx_t end_n = opts.dn;
                         idx_t end_x = opts.dx;
                         idx_t end_y = opts.dy;
                         idx_t end_z = opts.dz;
                         
                         // Modify begin and/or end based on direction.
                         if (nn == idx_t(MPIBufs::rank_prev)) { // neighbor is prev N.
                             begin_n = -hn; // begin at outside of halo.
                             end_n = 0;     // end at inside of halo.
                         }
                         if (nn == idx_t(MPIBufs::rank_next)) { // neighbor is next N.
                             begin_n = opts.dn; // begin at inside of halo.
                             end_n = opts.dn + hn; // end of outside of halo.
                         }
                         if (nx == idx_t(MPIBufs::rank_prev)) { // neighbor is on left.
                             begin_x = -hx;
                             end_x = 0;
                         }
                         if (nx == idx_t(MPIBufs::rank_next)) { // neighbor is on right.
                             begin_x = opts.dx;
                             end_x = opts.dx + hx;
                         }
                         if (ny == idx_t(MPIBufs::rank_prev)) { // neighbor is in front.
                             begin_y = -hy;
                             end_y = 0;
                         }
                         if (ny == idx_t(MPIBufs::rank_next)) { // neighbor is in back.
                             begin_y = opts.dy;
                             end_y = opts.dy + hy;
                         }
                         if (nz == idx_t(MPIBufs::rank_prev)) { // neighbor is above.
                             begin_z = -hz;
                             end_z = 0;
                         }
                         if (nz == idx_t(MPIBufs::rank_next)) { // neighbor is below.
                             begin_z = opts.dz;
                             end_z = opts.dz + hz;
                         }

                         // Add offsets and divide indices by vector
                         // lengths.  Begin/end vars shouldn't be negative
                         // (because we're always inside the halo), so '/'
                         // is ok.
                         idx_t begin_nv = (context.ofs_n + begin_n) / VLEN_N;
                         idx_t begin_xv = (context.ofs_x + begin_x) / VLEN_X;
                         idx_t begin_yv = (context.ofs_y + begin_y) / VLEN_Y;
                         idx_t begin_zv = (context.ofs_z + begin_z) / VLEN_Z;
                         idx_t end_nv = (context.ofs_n + end_n) / VLEN_N;
                         idx_t end_xv = (context.ofs_x + end_x) / VLEN_X;
                         idx_t end_yv = (context.ofs_y + end_y) / VLEN_Y;
                         idx_t end_zv = (context.ofs_z + end_z) / VLEN_Z;

                         // TODO: fix this when MPI + wave-front is enabled.
                         idx_t t = start_dt;
                         
                         // Define calc_halo to copy data from rcvBuf into main grid.
#define calc_halo(t,                                                    \
                  start_nv, start_xv, start_yv, start_zv,               \
                  stop_nv, stop_xv, stop_yv, stop_zv)  do {             \
                             idx_t nv = start_nv;                       \
                             idx_t xv = start_xv;                       \
                             idx_t yv = start_yv;                       \
                             idx_t izv = index_zv * step_zv;            \
                             for (idx_t zv = start_zv; zv < stop_zv; zv++) { \
                                 real_vec_t hval =                      \
                                     rcvBuf->readVecNorm(index_nv,      \
                                                         index_xv, index_yv, izv++, __LINE__); \
                                 gpd->writeVecNorm(hval, t, ARG_N(nv)   \
                                                   xv, yv, zv, __LINE__); \
                     } } while(0)

                         // Include auto-generated loops to invoke calc_halo() from
                         // begin_*v to end_*v;
#include "stencil_halo_loops.hpp"
#undef calc_halo
                     }
                 } );

        } // grids.

        double end_time = getTimeInSecs();
        context.mpi_time += end_time - start_time;
#endif
    }

    // Apply a function to each neighbor rank.
    // Called visitor function will contain the rank index of the neighbor.
    // The send and receive buffer pointers may be null.
    void MPIBufs::visitNeighbors(StencilContext& context,
                                 std::function<void (idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                                                     int rank,
                                                     Grid_NXYZ* sendBuf,
                                                     Grid_NXYZ* rcvBuf)> visitor)
    {
        for (idx_t nn = 0; nn < num_neighbors; nn++)
            for (idx_t nx = 0; nx < num_neighbors; nx++)
                for (idx_t ny = 0; ny < num_neighbors; ny++)
                    for (idx_t nz = 0; nz < num_neighbors; nz++)
                        if (context.my_neighbors[nn][nx][ny][nz] != MPI_PROC_NULL) {
                            visitor(nn, nx, ny, nz,
                                    context.my_neighbors[nn][nx][ny][nz],
                                    bufs[0][nn][nx][ny][nz],
                                    bufs[1][nn][nx][ny][nz]);
                        }
    }

    // Allocate new buffer in given direction and size.
    Grid_NXYZ* MPIBufs::allocBuf(int bd,
                                 idx_t nn, idx_t nx, idx_t ny, idx_t nz,
                                 idx_t dn, idx_t dx, idx_t dy, idx_t dz,
                                 const std::string& name,
                                 std::ostream& os)
    {
        // NB: there may be an existing buffer allocated here left
        // over from a shallow copy. Just ignore it and realloc.
        auto gp = getBuf(bd, nn, nx, ny, nz);
        *gp = new Grid_NXYZ(dn, dx, dy, dz,
                           0, 0, 0, 0,
                           0, 0, 0, 0,
                           name, true, os);
        assert(*gp);
        return *gp;
    }

#define ADD_1_OPTION(name, help1, help2, var, dim)                      \
    parser.add_option(new CommandLineParser::IdxOption                  \
                      (name #dim,                                       \
                       help1 " in '" #dim "' dimension" help2 ".",      \
                       var ## dim))
#define ADD_XYZ_OPTION(name, help, var) \
    ADD_1_OPTION(name, help, "", var, x); \
    ADD_1_OPTION(name, help, "", var, y);                               \
    ADD_1_OPTION(name, help, "", var, z);                               \
    parser.add_option(new CommandLineParser::MultiIdxOption             \
                      (name,                                            \
                       "Shorthand for -" name "x <integer> -" name      \
                       "y <integer> -" name "z <integer>.",             \
                       var ## x, var ## y, var ## z))
#define ADD_TXYZ_OPTION(name, help, var)                         \
    ADD_XYZ_OPTION(name, help, var);                             \
    ADD_1_OPTION(name, help, " (number of time steps)", var, t)
#define ADD_NXYZ_OPTION(name, help, var)                         \
    ADD_XYZ_OPTION(name, help, var);                             \
    ADD_1_OPTION(name, help, "", var, n)
#define ADD_TNXYZ_OPTION(name, help, var) \
    ADD_TXYZ_OPTION(name, help, var); \
    ADD_1_OPTION(name, help, "", var, n)

#ifdef USING_DIM_N
#define ADD_T_DIM_OPTION(name, help, var) \
    ADD_TNXYZ_OPTION(name, help, var)
#define ADD_DIM_OPTION(name, help, var) \
    ADD_NXYZ_OPTION(name, help, var)
#else
#define ADD_T_DIM_OPTION(name, help, var) \
    ADD_TXYZ_OPTION(name, help, var)
#define ADD_DIM_OPTION(name, help, var) \
    ADD_XYZ_OPTION(name, help, var)
#endif
    
    // Add these settigns to a cmd-line parser.
    void StencilSettings::add_options(CommandLineParser& parser)
    {
        ADD_T_DIM_OPTION("d", "Domain size for this rank", d);
        ADD_T_DIM_OPTION("r", "Region size", r);
        ADD_DIM_OPTION("b", "Block size", b);
        ADD_DIM_OPTION("g", "Block-group size", g);
        ADD_DIM_OPTION("p", "Extra memory-padding size", p);
#ifdef USE_MPI
        ADD_DIM_OPTION("nr", "Num ranks", nr);
        ADD_DIM_OPTION("ri", "This rank's logical index", ri);
        parser.add_option(new CommandLineParser::IntOption
                          ("msg_rank",
                           "Rank that will print informational messages.",
                           msg_rank));
#endif
        parser.add_option(new CommandLineParser::IntOption
                          ("max_threads",
                           "Max OpenMP threads to use.",
                           max_threads));
        parser.add_option(new CommandLineParser::IntOption
                          ("thread_divisor",
                           "Divide max OpenMP threads by <integer>.",
                           thread_divisor));
        parser.add_option(new CommandLineParser::IntOption
                          ("block_threads",
                           "Number of threads to use within each block.",
                           num_block_threads));
    }
    
    // Print usage message.
    void StencilSettings::print_usage(ostream& os,
                                      CommandLineParser& parser,
                                      const string& pgmName,
                                      const string& appNotes,
                                      const vector<string>& appExamples) const
    {
        os << "Usage: " << pgmName << " [options]\n"
            "Options:\n";
        parser.print_help(os);
        os << "Guidelines:\n"
            " Set block sizes to specify the amount of work done in each block.\n"
            "  A block size of 0 in a given dimension =>\n"
            "   block size is set to region size in that dimension.\n"
            "  Temporal cache-blocking is not yet supported, so effectively, bt = 1.\n"
            " Set block-group sizes to control in what order blocks are evaluated.\n"
            "  All blocks that fit within a block-group are evaluated before blocks\n"
            "   in the next block-group.\n"
            "  A block-group size of 0 in a given dimension =>\n"
            "   block-group size is set to block size in that dimension.\n"
            " Set region sizes to control temporal wave-front tile sizes.\n"
            "  The tempral region size should be larger than one, and\n"
            "   the spatial region sizes should be less than the rank-domain sizes\n"
            "   in at least one dimension to enable temporal wave-front tiling.\n"
            "  The spatial region sizes should be greater than block sizes\n"
            "   to enable threading withing each wave-front tile.\n"
            "  Control the time-steps in each temporal wave-front with -rt.\n"
            "   Special cases:\n"
            "    Using '-rt 1' disables wave-front tiling.\n"
            "    Using '-rt 0' => all time-steps in one wave-front.\n"
            "  A region size of 0 in a given dimension =>\n"
            "   region size is set to rank-domain size in that dimension.\n"
            " Set rank-domain sizes to specify the problem size done on this rank.\n"
            "  To 'weak-scale' this to a larger overall problem size, use multiple MPI ranks.\n"
#ifndef USE_MPI
            "  This binary has NOT been built with MPI support.\n"
#endif
            " So, rank-domain size >= region size >= block-group size >= block size.\n"
            " Controlling OpenMP threading:\n"
            "  Using '-max_threads 0' =>\n"
            "   max_threads is set to OpenMP's default number of threads.\n"
            "  The -thread_divisor option is a convenience to control the number of\n"
            "   hyper-threads used without having to know the number of cores,\n"
            "   e.g., using '-thread_divisor 2' will halve the number of OpenMP threads.\n"
            "  For stencil evaluation, threads are allocated using nested OpenMP:\n"
            "   Num blocks evaluated in parallel = max_threads / thread_divisor / block_threads.\n"
            "   Num threads per block = block_threads.\n" <<
            appNotes <<
            "Examples:\n" <<
            " " << pgmName << " -d 768 -dt 25\n" <<
            " " << pgmName << " -dx 512 -dy 256 -dz 128\n" <<
            " " << pgmName << " -d 2048 -dt 20 -r 512 -rt 10  # temporal tiling.\n" <<
            " " << pgmName << " -d 512 -npx 2 -npy 1 -npz 2   # multi-rank.\n";
        for (auto ae : appExamples)
            os << " " << pgmName << " " << ae << endl;
        os << flush;
    }
    
    // Make sure all user-provided settings are valid and finish setting up some
    // other vars before allocating memory.
    // Called from allocAll(), so it doesn't normally need to be called from user code.
    void StencilSettings::finalizeSettings(std::ostream& os) {

        // Round up domain size as needed.
        dt = roundUp(os, dt, CPTS_T, "rank domain size in t (time steps)");
        dn = roundUp(os, dn, CPTS_N, "rank domain size in n");
        dx = roundUp(os, dx, CPTS_X, "rank domain size in x");
        dy = roundUp(os, dy, CPTS_Y, "rank domain size in y");
        dz = roundUp(os, dz, CPTS_Z, "rank domain size in z");

        // Determine num regions.
        // Also fix up region sizes as needed.
        os << "\nRegions:" << endl;
        idx_t nrgt = findNumRegions(os, rt, dt, CPTS_T, "t");
        idx_t nrgn = findNumRegions(os, rn, dn, CPTS_N, "n");
        idx_t nrgx = findNumRegions(os, rx, dx, CPTS_X, "x");
        idx_t nrgy = findNumRegions(os, ry, dy, CPTS_Y, "y");
        idx_t nrgz = findNumRegions(os, rz, dz, CPTS_Z, "z");
        idx_t nrg = nrgt * nrgn * nrgx * nrgy * nrgz;
        os << " num-regions-per-rank: " << nrg << endl;

        // Determine num blocks.
        // Also fix up block sizes as needed.
        os << "\nBlocks:" << endl;
        idx_t nbt = findNumBlocks(os, bt, rt, CPTS_T, "t");
        idx_t nbn = findNumBlocks(os, bn, rn, CPTS_N, "n");
        idx_t nbx = findNumBlocks(os, bx, rx, CPTS_X, "x");
        idx_t nby = findNumBlocks(os, by, ry, CPTS_Y, "y");
        idx_t nbz = findNumBlocks(os, bz, rz, CPTS_Z, "z");
        idx_t nb = nbt * nbn * nbx * nby * nbz;
        os << " num-blocks-per-region: " << nb << endl;

        // Adjust defaults for block-groups.
        if (!gn) gn = bn;
        if (!gx) gx = bx;
        if (!gy) gy = by;
        if (!gz) gz = bz;

        // Determine num groups.
        // Also fix up group sizes as needed.
        os << "\nBlock-groups:" << endl;
        idx_t ngn = findNumGroups(os, gn, rn, bn, "n");
        idx_t ngx = findNumGroups(os, gx, rx, bx, "x");
        idx_t ngy = findNumGroups(os, gy, ry, by, "y");
        idx_t ngz = findNumGroups(os, gz, rz, bz, "z");
        idx_t ng = ngn * ngx * ngy * ngz;
        os << " num-block-groups-per-region: " << ng << endl;

        // Round up padding as needed.
        pn = roundUp(os, pn, VLEN_N, "extra padding in n");
        px = roundUp(os, px, VLEN_X, "extra padding in x");
        py = roundUp(os, py, VLEN_Y, "extra padding in y");
        pz = roundUp(os, pz, VLEN_Z, "extra padding in z");

    }



} // namespace yask.
