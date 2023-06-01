/*****************************************************************************

YASK: Yet Another Stencil Kit
Copyright (c) 2014-2023, Intel Corporation

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

///////// Methods for Vars, etc. ////////////

#include "Print.hpp"
#include "ExprUtils.hpp"
#include "Var.hpp"
#include "Print.hpp"
#include "CppIntrin.hpp"

namespace yask {


    // var APIs.
    yc_var_point_node_ptr
    Var::new_var_point(const std::vector<yc_number_node_ptr>& index_exprs) {

        // Check for correct number of indices.
        if (_vdims.size() != index_exprs.size()) {
            FORMAT_AND_THROW_YASK_EXCEPTION("attempt to create a var point in " <<
                                            _vdims.size() << "D var '" << _name << "' with " <<
                                            index_exprs.size() << " index expressions");
        }

        // Make args.
        num_expr_ptr_vec args;
        for (size_t i = 0; i < _vdims.size(); i++) {
            auto p = dynamic_pointer_cast<NumExpr>(index_exprs.at(i));
            assert(p);
            args.push_back(p->clone());
        }

        // Create a point from the args.
        var_point_ptr gpp = make_shared<VarPoint>(this, args);
        return gpp;
    }

    yc_var_point_node_ptr
    Var::new_relative_var_point(const std::vector<int>& dim_offsets) {

        // Check for correct number of indices.
        if (_vdims.size() != dim_offsets.size()) {
            FORMAT_AND_THROW_YASK_EXCEPTION("attempt to create a relative var point in " <<
                                            _vdims.size() << "D var '" << _name << "' with " <<
                                            dim_offsets.size() << " indices");
        }

        // Check dim types.
        // Make default args w/just index.
        num_expr_ptr_vec args;
        for (size_t i = 0; i < _vdims.size(); i++) {
            auto dim = _vdims.at(i);
            if (dim->get_type() == MISC_INDEX) {
                FORMAT_AND_THROW_YASK_EXCEPTION("attempt to create a relative var point in " <<
                                                _vdims.size() << "D var '" << _name <<
                                                "' containing non-step or non-domain dim '" <<
                                                dim->_get_name() << "'");
            }
            auto ie = dim->clone();
            args.push_back(ie);
        }

        // Create a point from the args.
        var_point_ptr gpp = make_shared<VarPoint>(this, args);

        // Set the offsets, which creates a new
        // expression for each index.
        for (size_t i = 0; i < _vdims.size(); i++) {
            auto dim = _vdims.at(i);
            IntScalar ofs(dim->_get_name(), dim_offsets.at(i));
            gpp->set_arg_offset(ofs);
        }
        return gpp;
    }

    vector<string> Var::get_dim_names() const {
        vector<string> ret;
        for (auto dn : get_dims())
            ret.push_back(dn->_get_name());
        return ret;
    }

    // Ctor for Var.
    Var::Var(Solution* soln,
             string name,
             bool is_scratch,
             const index_expr_ptr_vec& dims) :
        _soln(soln),
        _name(name),       // TODO: validate that name is legal C++ var.
        _is_scratch(is_scratch)
    {
        assert(soln);

        // Name already used?
        auto& vars = soln->_get_vars();
        for (auto gp : vars) {
            if (gp->_get_name() == name)
                THROW_YASK_EXCEPTION("var name '" + name + "' already used");
        }

        // Register in soln.
        vars.insert(this);

        // Define dims.
        _vdims = dims;
    }

    // Simple accessors.
    CompilerSettings& Var::get_settings() { return _soln->get_settings(); }
    const Dimensions& Var::get_soln_dims() { return _soln->get_dims(); }

    // Determine whether var can be folded.
    void Var::set_dim_counts(const Dimensions& dims) {

        // Find num of dim types in this var.
        _num_step_dims = 0;
        _num_domain_dims = 0;
        _num_misc_dims = 0;
        _num_foldable_dims = 0;
        for (auto gdim : _vdims) {
            auto& dname = gdim->_get_name();
            auto dtype = gdim->get_type();

            if (dtype == STEP_INDEX)
                _num_step_dims++;

            else if (dtype == DOMAIN_INDEX) {
                _num_domain_dims++;
                if (dims._fold_gt1.lookup(dname))
                    _num_foldable_dims++;
            }

            else if (dtype == MISC_INDEX) {
                _num_misc_dims++;
            }

            else
                assert("internal error: unknown dim type");
        }

        // Never fold vars without domain dims, even if there is no vectorization.
        if (_num_domain_dims == 0)
            _is_foldable = false;

        // Otherwise, can fold if ALL vec dims are used in this var.
        else {

            // NB: this will be true if there is no vectorization, i.e.,
            // both are zero.  We do this because the compiler expects stencils
            // to be vectorizable.
            _is_foldable = _num_foldable_dims == int(dims._fold_gt1.size());
        }
    }

    // Determine size of the misc space.
    // This is the product of all the observed misc ranges.
    int Var::get_misc_space_size() const {

        int msz = 1;
        for (auto& dim : _vdims) {
            auto dtype = dim->get_type();
            if (dtype == MISC_INDEX) {
                auto& dname = dim->_get_name();
                auto* minp = _min_indices.lookup(dname);
                auto* maxp = _max_indices.lookup(dname);
                if (minp && maxp) {
                    assert(*maxp >= *minp);
                    int sz = *maxp - *minp + 1;
                    msz *= sz;
                }
            }
        }
        return msz;
    }
    
    // Determine whether halo sizes are equal.
    bool Var::is_halo_same(const Var& other) const {

        // Same dims?
        if (!are_dims_same(other))
            return false;

        // Same halos?
        for (auto& dim : _vdims) {
            auto& dname = dim->_get_name();
            auto dtype = dim->get_type();
            if (dtype == DOMAIN_INDEX) {
                for (bool left : { false, true }) {
                    int sz = get_halo_size(dname, left);
                    int osz = other.get_halo_size(dname, left);
                    if (sz != osz)
                        return false;
                }
            }
        }
        return true;
    }

    // Update halos and L1 dist based on those in 'other' var.
    // Halos are updated at corresponding stages, L-R sides, and steps.
    // This var's halos and L1 dist can only be increased.
    bool Var::update_halo(const Var& other) {
        assert(are_dims_same(other));
        bool changed = false;

        // Loop thru other var's halo values.
        for (auto& hi : other._halos) {
            auto& stname = hi.first;
            auto& h2 = hi.second;
            for (auto& i0 : h2) {
                auto& left = i0.first;
                auto& m1 = i0.second;
                for (auto& i1 : m1) {
                    auto& step = i1.first;
                    const IntTuple& ohalos = i1.second;
                    for (auto& dim : ohalos) {
                        auto& dname = dim._get_name();
                        auto& val = dim.get_val();

                        // Any existing value in this var?
                        auto& halos = _halos[stname][left][step];
                        auto* p = halos.lookup(dname);

                        // If not, add this one.
                        if (!p) {
                            halos.add_dim_back(dname, val);
                            changed = true;
                        }

                        // Keep larger value.
                        else if (val > *p) {
                            *p = val;
                            changed = true;
                        }

                        // Else, current value is larger than val, so don't update.
                    }
                }
            }
        }
        update_l1_dist(other._l1_dist);
        return changed;
    }

    // Update halos based on each value in 'offsets' in some
    // read or write to this var.
    // This var's halos can only be increased.
    bool Var::update_halo(const string& stage_name, const IntTuple& offsets) {
        bool changed = false;

        // Find step value or use 0 if none.
        int step_val = 0;
        auto step_dim = get_step_dim();
        if (step_dim) {
            auto* p = offsets.lookup(step_dim->_get_name());
            if (p)
                step_val = *p;
        }

        // Manhattan dist of halo.
        int l1_dist = 0;

        // Update halo vals.
        for (auto& dim : offsets) {
            auto& dname = dim._get_name();
            int val = dim.get_val();
            bool left = val <= 0;
            auto& halos = _halos[stage_name][left][step_val];

            // Don't keep halo in step dim.
            if (step_dim && dname == step_dim->_get_name())
                continue;

            // Store abs value (neg values are on "left").
            val = abs(val);

            // Track num dims.
            if (val > 0)
                l1_dist++;
            
            // Any existing value?
            auto* p = halos.lookup(dname);

            // If not, add this one.
            if (!p) {
                halos.add_dim_back(dname, val);
                changed = true;
            }

            // Keep larger value.
            else if (val > *p) {
                *p = val;
                changed = true;
            }

            // Else, current value is larger than val, so don't update.
        }

        // Update L1.
        update_l1_dist(l1_dist);

        return changed;
    }

    // Update write stages and offsets.
    void Var::update_write_points(const string& stage_name, const IntTuple& offsets) {
        auto& sdims = get_soln_dims();
        auto* sofs = offsets.lookup(sdims._step_dim);
        if (sofs)
            _write_points[stage_name] = *sofs;
    }
    
    // Update const indices based on 'indices'.
    void Var::update_const_indices(const IntTuple& indices) {

        for (auto& dim : indices) {
            auto& dname = dim._get_name();
            int val = dim.get_val();

            // Update min.
            auto* minp = _min_indices.lookup(dname);
            if (!minp)
                _min_indices.add_dim_back(dname, val);
            else if (val < *minp)
                *minp = val;

            // Update max.
            auto* maxp = _max_indices.lookup(dname);
            if (!maxp)
                _max_indices.add_dim_back(dname, val);
            else if (val > *maxp)
                *maxp = val;
        }
    }

    // Determine how many values in step-dim are needed.
    Var::StepDimInfo Var::get_step_dim_info() const
    {
        StepDimInfo sdi;
        
        // No step-dim index used.
        auto step_dim = get_step_dim();
        if (!step_dim)
            return sdi;

        // No info stored?
        if (_halos.size() == 0)
            return sdi;

        // Need the max across all stages.
        int max_sz = 1;

        // Loop thru each stage w/halos, including halos w/size zero.
        for (auto& hi : _halos) {
            auto& stage_name = hi.first;
            auto& h2 = hi.second;
            #ifdef DEBUG_HALOS
            cout << "* var " << get_name() << " in " << stage_name << endl;
            #endif

            // Written?
            bool is_written = false;

            // First (lowest) and last (highest) step-dim offset.
            const int unset = -9999;
            int first_ofs = unset, last_ofs = unset;

            // left and right.
            for (auto& i : h2) {
                //auto left = i.first;
                auto& h3 = i.second; // map of step-dims to halos.

                // Step-dim offsets.
                for (auto& j : h3) {
                    auto ofs = j.first;
                    auto& halo = j.second; // halo tuple at step-val 'ofs'.

                    // Written here?
                    if (_write_points.count(stage_name) && _write_points.at(stage_name) == ofs)
                        is_written = true;

                    // Any existing value?
                    if (halo.size()) {

                        // Update vars.
                        if (first_ofs == unset)
                            first_ofs = last_ofs = ofs;
                        else {
                            first_ofs = std::min(first_ofs, ofs);
                            last_ofs = std::max(last_ofs, ofs);
                        }
                    }
                }
            }
            #ifdef DEBUG_HALOS
            print_halos(cout, "** ");
            cout << "*** halo range: [" << first_ofs <<
                "..." << last_ofs << "] in stage " << stage_name << endl;
            #endif

            // Only need to process if >1 offset.
            if (last_ofs != unset && first_ofs != unset && last_ofs != first_ofs) {

                // Default step-dim size is range of step offsets.
                // For example, if equation touches 't-1' through 't+2',
                // 'sz' is 4.
                int sz = last_ofs - first_ofs + 1;
                #ifdef DEBUG_HALOS
                cout << "*** initial sz = " << sz << endl;
                #endif

                // Check for possible writeback.
                if (is_written) {

                    // First and last largest halos.
                    int first_max_halo = 0, last_max_halo = 0;
                    for (auto& i : h2) {
                        //auto left = i.first;
                        auto& h3 = i.second; // map of step-dims to halos.
                        
                        if (h3.count(first_ofs) && h3.at(first_ofs).size())
                            first_max_halo = std::max(first_max_halo, h3.at(first_ofs).max());
                        if (h3.count(last_ofs) && h3.at(last_ofs).size())
                            last_max_halo = std::max(last_max_halo, h3.at(last_ofs).max());
                    }

                    // If first and last halos are zero, we can further optimize
                    // storage by immediately reusing memory location.
                    if (sz > 1 && first_max_halo == 0 && last_max_halo == 0) {
                        int write_ofs = _write_points.at(stage_name);
                        sz--;
                        if (last_ofs == write_ofs) // forward step.
                            sdi.writeback_ofs[stage_name] = first_ofs; // replace lowest read.
                        else if (first_ofs == write_ofs) // backward step.
                            sdi.writeback_ofs[stage_name] = last_ofs; // replace lowest read.
                        else
                            assert("write ofs is neither first or last");
                        #ifdef DEBUG_HALOS
                        cout << "*** optimized sz = " << sz << endl;
                        #endif
                    }
                }

                // Keep max so far.
                max_sz = std::max(max_sz, sz);
            }

        } // stages.
        #ifdef DEBUG_HALOS
        cout << "* final sz = " << sz << endl;
        #endif

        // Override by API.
        if (_step_alloc > 0)
            sdi.step_dim_size = _step_alloc;

        // Specified on cmd line.
        if (_soln->get_settings()._step_alloc > 0)
            sdi.step_dim_size = _soln->get_settings()._step_alloc;

        sdi.step_dim_size = max_sz;
        return sdi;
    }

    // Description of this var.
    string Var::get_descr() const {
        string d = _name + "(";
        int i = 0;
        for (auto dn : get_dims()) {
            if (i++) d += ", ";
            d += dn->_get_name();
        }
        d += ")";
        return d;
    }

    void Vars::set_dim_counts() {
            for (auto gp : _vars)
                gp->set_dim_counts(_soln->get_dims());
        }

} // namespace yask.
