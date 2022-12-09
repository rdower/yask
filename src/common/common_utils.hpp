/*****************************************************************************

YASK: Yet Another Stencil Kit
Copyright (c) 2014-2022, Intel Corporation

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
#pragma once

//////// Some common code shared between YASK compiler and kernel. //////////

// Include this first to assure NDEBUG is set properly.
#include "yask_assert.hpp"

#include "yask_common_api.hpp"
#include <cmath>
#include <cfloat>
#include <set>
#include <vector>
#include <map>
#include <functional>

// This must come before including the API header to make sure
// _OPENMP is defined.
#ifdef _OPENMP
#include <omp.h>
#if !defined(KMP_VERSION_MAJOR) || KMP_VERSION_MAJOR >= 5
// omp_set_nested() is deprecated.
#define omp_set_nested(n) void(0)
#endif
#else
typedef int omp_lock_t;
inline int omp_get_num_procs() { return 1; }
inline int omp_get_num_threads() { return 1; }
inline int omp_get_max_threads() { return 1; }
inline int omp_get_thread_num() { return 0; }
inline void omp_set_num_threads(int n) { }
inline void omp_set_nested(int n) { }
inline int omp_get_max_active_levels() { return 1; }
inline void omp_set_max_active_levels(int n) { }
inline int omp_get_level() { return 0; }
inline void omp_init_lock(omp_lock_t* p) { }
inline bool omp_set_lock(omp_lock_t* p) { return true; }
inline void omp_unset_lock(omp_lock_t* p) { }
#endif

// Rounding macros for integer types.
#define CEIL_DIV(numer, denom) (((numer) + (denom) - 1) / (denom))
#define ROUND_UP(n, mult) (CEIL_DIV(n, mult) * (mult))
#define ROUND_DOWN(n, mult) (((n) / (mult)) * (mult))

// Macro for throwing yask_exception with a string.
// Example: THROW_YASK_EXCEPTION("all your base are belong to us");
#define THROW_YASK_EXCEPTION(message) do {                          \
        auto msg = std::string("YASK error: ") + message;           \
        yask_exception e(msg);                                      \
        throw e;                                                    \
    } while(0)

// Macro for creating a string and throwing yask_exception with it.
// Example: FORMAT_AND_THROW_YASK_EXCEPTION("bad value: x = " << x);
#define FORMAT_AND_THROW_YASK_EXCEPTION(message) do {               \
        std::stringstream err;                                      \
        err << message;                                             \
        THROW_YASK_EXCEPTION(err.str());                            \
    } while(0)

namespace yask {

    // Controls whether make*Str() functions add
    // suffixes or just print full number for
    // machine parsing.
    extern bool is_suffix_print_enabled;
    
    // Return num with SI multiplier and "i_b" suffix,
    // e.g., 41.2KiB.
    extern std::string make_byte_str(size_t nbytes);

    // Return num with SI multiplier, e.g., 4.23M.
    extern std::string make_num_str(idx_t num);
    extern std::string make_num_str(double num);

    // Divide 'num' equally into 'nparts'.
    // Returns the size of the 'n'th part,
    // where 0 <= 'n' < 'nparts'.
    // Example:
    //  div_equally_size_n(6, 4, 0) returns 2.
    //  div_equally_size_n(6, 4, 1) returns 2.
    //  div_equally_size_n(6, 4, 2) returns 1.
    //  div_equally_size_n(6, 4, 3) returns 1.
    template <typename T>
    inline T div_equally_size_n(T num, T nparts, T n) {
        host_assert(n >= 0);
        host_assert(n < nparts);
        T p = num / nparts;
        T rem = num % nparts;
        p += (n < rem) ? 1 : 0;
        return p;
    }
    
    // Divide 'num' equally into 'nparts'.
    // Returns the *cumulative* sizes of the 0-'n'th parts,
    // if 0 <= 'n' < 'nparts' and 0 if n < 0.
    // The <0 case is handy for calculating the initial
    // starting point when passing 'n'-1 and 'n'==0.
    // Example:
    //  div_equally_cumu_size_n(6, 4, -1) returns 0.
    //  div_equally_cumu_size_n(6, 4, 0) returns 2.
    //  div_equally_cumu_size_n(6, 4, 1) returns 4.
    //  div_equally_cumu_size_n(6, 4, 2) returns 5.
    //  div_equally_cumu_size_n(6, 4, 3) returns 6.
    template <typename T>
    inline T div_equally_cumu_size_n(T num, T nparts, T n) {
        if (n < 0)
            return 0;
        host_assert(n >= 0);
        host_assert(n < nparts);
        T p = (num / nparts) * (n + 1);
        T rem = num % nparts;
        p += (n < rem) ? (n + 1) : rem;
        return p;
    }
    
    // Divide 'num' equally into 'nparts'.
    // Returns size of all parts.
    // Example: div_equally_all_sizes(6, 4) returns <2, 2, 1, 1>.
    template <typename T>
    inline std::vector<T> div_equally_all_sizes(T num, T nparts) {
        std::vector<T> p(nparts, num / nparts);
        for (T i = 0; i < num % nparts; i++)
            p[i]++;
        return p;
    }

    // A var that behaves like OMP_NUM_THREADS to specify the
    // default number of threads in each level.
    // TODO: try to remove the need for these vars by using
    // OMP APIs to discover the nesting levels and num threads.
    constexpr int yask_max_levels = 2;
    extern int yask_num_threads[];

    // Get number of threads that will execute a yask_parallel_for() loop
    // based on the current OpenMP nesting level.
    inline int yask_get_num_threads() {

        // Nested parallel regions.
        if (omp_get_max_active_levels() > 1 &&
            yask_num_threads[0] > 0 &&
            yask_num_threads[1] > 1)
            return yask_num_threads[0] * yask_num_threads[1];

        // Single parallel region.
        else if (yask_num_threads[0] > 0)
            return yask_num_threads[0];

        // YASK thread vars not set; use OMP val.
        else
            return omp_get_num_threads();
    }

    // Execute a nested OMP for loop as if it was a single loop.
    // 'start' will be 'begin', 'begin'+'stride', 'begin'+2*'stride', etc.
    // 'stop' will be 'begin'+'stride', etc.
    // 'thread_num' will be a unique number across the nested threads.
    //#define DEBUG_PAR_FOR
    inline void yask_parallel_for(idx_t begin, idx_t end, idx_t stride,
                                  std::function<void (idx_t start, idx_t stop,
                                                      idx_t thread_num)> visitor) {
        if (end <= begin)
            return;

        // Number of iterations in canonical loop.
        idx_t niter = CEIL_DIV(end - begin, stride);
        #ifdef DEBUG_PAR_FOR
        std::cout << "** yask_parallel_for: [" << begin << "..." << end << ") by " << stride <<
            ": " << niter << " iters\n";
        #endif

        // Only 1 value.
        if (niter == 1) {
            visitor(begin, end, 0);
            return;
        }
        
        #ifndef _OPENMP
        // Canonical sequential loop.
        for (idx_t i = begin; i < end; i += stride) {
            idx_t stop = std::min(i + stride, end);
            idx_t tn = omp_get_thread_num();
            visitor(i, stop, tn);
        }
        #else

        // Non-nested parallel.
        if (omp_get_max_active_levels() < 2 ||
            yask_num_threads[0] <= 0 ||
            yask_num_threads[1] <= 1 ||
            niter <= yask_num_threads[0]) {

            if (yask_num_threads[0] > 0)
                omp_set_num_threads(yask_num_threads[0]);
            #pragma omp parallel for schedule(static)
            for (idx_t i = begin; i < end; i += stride) {
                idx_t stop = std::min(i + stride, end);
                idx_t tn = omp_get_thread_num();
                visitor(i, stop, tn);
            }
        }

        // Nested parallel.
        else {

            // Number of outer threads.
            idx_t nthr0 = yask_num_threads[0];
            assert(nthr0 > 0);
            omp_set_num_threads(nthr0);

            // Outer parallel region.
            #pragma omp parallel
            {
                idx_t n0 = omp_get_thread_num();

                // Calculate begin and end points for this thread.
                idx_t tbegin = div_equally_cumu_size_n(niter, nthr0, n0 - 1) * stride;
                idx_t tend = div_equally_cumu_size_n(niter, nthr0, n0) * stride;

                #ifdef DEBUG_PAR_FOR
                #pragma omp critical
                std::cout << "** outer thread " << n0 << ": [" << tbegin << "..." << tend << ") by " <<
                    stride << "\n" << std::flush;
                #endif
                assert(tend >= tbegin);

                // Nothing to do?
                if (tend <= tbegin) {
                }

                // Only need one in this thread?
                else if (tend - tbegin == 1)
                    visitor(tbegin, tend, n0);

                else {

                    // Set number of threads for the nested OMP loop.
                    // (Doesn't seem to work w/g++ 8.2.0: just starts 1 nested
                    // thread if nthr0 > 1.)
                    idx_t nthr1 = yask_num_threads[1];
                    assert(nthr1 > 1);
                    omp_set_num_threads(nthr1);

                    #ifdef DEBUG_PAR_FOR
                    // Test OMP region w/o for loop.
                    #pragma omp parallel
                    {
                        idx_t n1 = omp_get_thread_num();
                        idx_t thread_num = n0 * nthr1 + n1;
                        #pragma omp critical
                        std::cout << "** thread " << thread_num <<
                            "(" << n0 << ":" << n1 <<
                            ")\n" << std::flush;
                    }
                    #endif
                    
                    // Inner parallel loop over elements.
                    #pragma omp parallel for schedule(static)
                    for (idx_t i = tbegin; i < tend; i += stride) {
                        idx_t stop = std::min(i + stride, tend);
                        idx_t n1 = omp_get_thread_num();
                        idx_t thread_num = n0 * nthr1 + n1;
                        #ifdef DEBUG_PAR_FOR
                        #pragma omp critical
                        std::cout << "** thread " << thread_num <<
                            "(" << n0 << ":" << n1 <<
                            "): [" << i << "..." << stop << ") by " <<
                            stride << "\n" << std::flush;
                        #endif
                        visitor(i, stop, thread_num);
                    }
                }
            }
        }
        #endif        
    }

    // Sequential version of yask_parallel_for().
    inline void yask_for(idx_t begin, idx_t end, idx_t stride,
                         std::function<void (idx_t start, idx_t stop,
                                             idx_t thread_num)> visitor) {
        if (end <= begin)
            return;

        // Canonical sequential loop.
        for (idx_t i = begin; i < end; i += stride) {
            idx_t stop = std::min(i + stride, end);
            idx_t tn = omp_get_thread_num();
            visitor(i, stop, tn);
        }
    }
    

    // Set that retains order of things added.
    // Or, vector that allows insertion if element doesn't exist.
    template <typename T>
    class vector_set final {
        std::vector<T> _items;     // no duplicates.
        std::map<T, size_t> _posn; // _posn[_items[i]] = i;

    public:
        vector_set() {}
        ~vector_set() {}

        // Default assign and copy ctor are okay.

        // STL methods.
        // Do not provide any non-const iterators or element access to prevent
        // breaking _items <-> _posn relationship.
        typename std::vector<T>::const_iterator begin() const {
            return _items.begin();
        }
        typename std::vector<T>::const_iterator end() const {
            return _items.end();
        }
        const T& at(size_t i) const {
            return _items.at(i);
        }
        const T& operator[](size_t i) const {
            return _items[i];
        }
        const T& front() const {
            return _items.front();
        }
        const T& back() const {
            return _items.back();
        }
        size_t size() const {
            assert(_items.size() == _posn.size());
            return _items.size();
        }
        bool empty() const {
            return size() == 0 ;
        }
        size_t count(const T& val) const {
            assert(_items.size() == _posn.size());
            return _posn.count(val);
        }
        void insert(const T& val) {
            assert(_items.size() == _posn.size());
            if (_posn.count(val) == 0) {
                _items.push_back(val);
                _posn[val] = _items.size() - 1;
            }
            assert(_items.size() == _posn.size());
        }
        void push_back(const T& val) {
            insert(val);        // Does nothing if already exists.
        }
        void erase(const T& val) {
            if (_posn.count(val) > 0) {
                size_t op = _posn.at(val);
                _items.erase(_items.begin() + op);

                // Repair positions of items after 'val'.
                for (auto pi : _posn) {
                    auto& p = pi.second;
                    if (p > op)
                        p--;
                }
                _posn.erase(val);
            }
            assert(_items.size() == _posn.size());
        }
        void clear() {
            _items.clear();
            _posn.clear();
        }

        // New methods.
        void swap(size_t i, size_t j) {
            assert(i < _items.size());
            assert(j < _items.size());
            if (i == j)
                return;
            T tmp = _items[i];
            _items[i] = _items[j];
            _items[j] = tmp;
            _posn[_items[i]] = i;
            _posn[_items[j]] = j;
        }
    };

    // A class for maintaining elapsed time.
    // NOT a virtual class.
    // Example:
    //   time --->
    //     start() ... stop() ... start() ... stop() ... get_elapsed_time()
    //     |   A secs  |          |   B secs  |
    // 1st call to stop() returns A.
    // 2nd call to stop() returns B.
    // Call to get_elapsed_time() returns A + B.
    class YaskTimer {

        /* struct timespec {
           time_t   tv_sec;        // seconds
           long     tv_nsec;       // nanoseconds
           };
        */
        struct timespec _begin, _elapsed;

    public:

        typedef struct timespec TimeSpec;

        YaskTimer() { clear(); }
        ~YaskTimer() { }

        // Reset elapsed time to zero.
        void clear() {
            _begin.tv_sec = _elapsed.tv_sec = 0;
            _begin.tv_nsec = _elapsed.tv_nsec = 0;
        }

        // Make a current timespec to be provided to start() or stop().
        // This allows multiple timers to use the same timespec.
        static TimeSpec get_timespec() {
            TimeSpec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            return ts;
        }

        // Start a timed region.
        // start() and stop() can be called multiple times in
        // pairs before calling get_elapsed_secs(), which
        // will return the cumulative time over all timed regions.
        void start(const TimeSpec& ts);
        void start() {
            auto ts = get_timespec();
            start(ts);
        }

        // End a timed region.
        // Return time since previous call to start(); this is *not*
        // generally the same as the value returned by get_elapsed_secs().
        double stop(const TimeSpec& ts);
        double stop() {
            auto ts = get_timespec();
            return stop(ts);
        }

        // Get elapsed time between all preceding start/stop pairs since
        // object creation or previous call to clear().  Does not reset
        // value, so it may be used for querying cumulative time.
        double get_elapsed_secs() const {

            // Make sure timer was stopped.
            assert(_begin.tv_sec == 0);

            return double(_elapsed.tv_sec) + double(_elapsed.tv_nsec) * 1e-9;
        }

        // Get elapsed time since previous start.
        // Used to check time w/o stopping timer.
        double get_secs_since_start() const;
    };

    // A class to parse command-line args.
    class CommandLineParser {

    public:

        // Base class for a command-line option.
        class OptionBase {
        protected:
            std::string _name;
            std::string _help;
            std::string _help_leader;
            std::string _current_value_str;

            // Internal function to print help.
            virtual void _print_help(std::ostream& os,
                                     const std::string& str,
                                     int width) const;

            // Check for matching option to str at args[argi].
            // Return true and increment argi if match.
            virtual bool _is_opt(const string_vec& args, int& argi,
                                 const std::string& str) const;

            // Get one double value from args[argi++].
            // Exit on failure.
            virtual double _double_val(const string_vec& args, int& argi);

            // Get one idx_t value from args[argi++].
            // Exit on failure.
            virtual idx_t _idx_val(const string_vec& args, int& argi);

            // Get one string value from args[argi++].
            // Exit on failure.
            virtual std::string _string_val(const string_vec& args, int& argi);

        public:
            OptionBase(const std::string& name,
                       const std::string& help_msg) :
                _name(name), _help(help_msg),
                _help_leader("    "),
                _current_value_str("Current value = ")
            { }
            virtual ~OptionBase() { }

            // Accessors.
            virtual const std::string& get_name() const {
                return _name;
            }
            virtual const std::string& get_help() const {
                return _help;
            }

            // Print help on this option.
            virtual void print_help(std::ostream& os,
                                    int width) const {
                _print_help(os, _name, width);
            }

            // Print current value of this option.
            virtual std::ostream& print_value(std::ostream& os) const =0;

            // Check for matching option and any needed args at args[argi].
            // Return true, set val, and increment argi if match.
            virtual bool check_arg(const string_vec& args, int& argi) =0;
        };
        typedef std::shared_ptr<OptionBase> OptionPtr;

        // A boolean option.
        class BoolOption : public OptionBase {
            bool& _val;

        public:
            BoolOption(const std::string& name,
                       const std::string& help_msg,
                       bool& val) :
                OptionBase(name, help_msg), _val(val) { }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                os << (_val ? "true" : "false");
                return os;
            }
            virtual bool check_arg(const string_vec& args, int& argi) override;
        };

        // An int option.
        class IntOption : public OptionBase {
            int& _val;

        public:
            IntOption(const std::string& name,
                      const std::string& help_msg,
                      int& val) :
                OptionBase(name, help_msg), _val(val) { }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                os << _val;
                return os;
            }
            virtual bool check_arg(const string_vec& args, int& argi) override;
        };

        // A double option.
        class DoubleOption : public OptionBase {
            double& _val;

        public:
            DoubleOption(const std::string& name,
                      const std::string& help_msg,
                      double& val) :
                OptionBase(name, help_msg), _val(val) { }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                os << _val;
                return os;
            }
            virtual bool check_arg(const string_vec& args, int& argi) override;
        };

        // An idx_t option.
        class IdxOption : public OptionBase {
            idx_t& _val;

        public:
            IdxOption(const std::string& name,
                       const std::string& help_msg,
                       idx_t& val) :
                OptionBase(name, help_msg), _val(val) { }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                os << _val;
                return os;
            }
            virtual bool check_arg(const string_vec& args, int& argi) override;
        };

        // An idx_t option that sets multiple vars.
        class MultiIdxOption : public OptionBase {
            std::vector<idx_t*> _vals;

        public:
            MultiIdxOption(const std::string& name,
                           const std::string& help_msg,
                           std::vector<idx_t*> vals) :
                OptionBase(name, help_msg), _vals(vals) {
                _current_value_str = "Current values = ";
            }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                for (size_t i = 0; i < _vals.size(); i++) {
                    if (i > 0)
                        os << ", ";
                    os << *_vals[i];
                }
                return os;
            }
            virtual bool check_arg(const string_vec& args,
                                   int& argi) override;
        };

        // A string option.
        class StringOption : public OptionBase {
            std::string& _val;

        public:
            StringOption(const std::string& name,
                         const std::string& help_msg,
                         std::string& val) :
                OptionBase(name, help_msg), _val(val) { }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                os << "'" << _val << "'";
                return os;
            }
            virtual bool check_arg(const string_vec& args,
                                   int& argi) override;
        };

        // A list-of-strings option.
        class StringListOption : public OptionBase {
            std::set<std::string> _allowed_strs; // empty to allow any strings.
            string_vec& _val;

        public:
            StringListOption(const std::string& name,
                             const std::string& help_msg,
                             string_vec& val) :
                OptionBase(name, help_msg),
                _val(val) { }
            StringListOption(const std::string& name,
                             const std::string& help_msg,
                             const std::set<std::string>& allowed_strs,
                             string_vec& val) :
                OptionBase(name, help_msg),
                _allowed_strs(allowed_strs),
                _val(val) { }

            virtual void print_help(std::ostream& os,
                                    int width) const override;
            virtual std::ostream& print_value(std::ostream& os) const override {
                int n = 0;
                for (auto& v : _val) {
                    if (n)
                        os << ",";
                    os << v;
                    n++;
                }
                return os;
            }
            virtual bool check_arg(const string_vec& args, int& argi) override;
        };

    protected:
        std::map<std::string, OptionPtr> _opts;
        int _width = 78;

    public:

        // Ctor.
        CommandLineParser() { }

        // Dtor.
        virtual ~CommandLineParser() { }

        // Tokenize args from a string.
        static string_vec set_args(const std::string& arg_string);

        // Set help width.
        virtual void set_width(int width) {
            _width = width;
        }

        // Add an allowed option.
        virtual void add_option(OptionPtr opt) {
            _opts[opt->get_name()] = opt;
        }

        // Print help info on all options.
        virtual void print_help(std::ostream& os) const;

        // Print current settings of all options.
        virtual void print_values(std::ostream& os) const;

        // Parse options from 'args' and set corresponding vars.
        // Recognized strings from args are consumed, and unused ones
        // remain for further processing by the application.
        virtual std::string parse_args(const std::string& pgm_name,
                                       const string_vec& args);

        // Same as above, but splits 'arg_string' into tokens.
        virtual std::string parse_args(const std::string& pgm_name,
                                       const std::string& arg_string) {
            auto args = set_args(arg_string);
            return parse_args(pgm_name, args);
        }

        // Same as above, but pgm_name is populated from argv[0]
        // and rest of argv is parsed.
        virtual std::string parse_args(int argc, char** argv) {
            std::string pgm_name = argv[0];
            string_vec args;
            for (int i = 1; i < argc; i++)
                args.push_back(argv[i]);
            return parse_args(pgm_name, args);
        }
    };
    
} // namespace.

