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

//////// Methods for output object. //////////

#include "yask_common_api.hpp"
#include <sstream>
#include <assert.h>

using namespace std;

namespace yask {

    // Release process:
    // - Update version if needed.
    // - Set is_alpha to false.
    // - Push changes to 'develop' branch.
    // - Merge into 'master' branch.
    // - Create release on github.
    // - Increment last digit in version.
    // - Set is_alpha to true;
    // - Push changes to 'develop' branch.
    
    const string version = "2.00.01";
    const bool is_alpha = true;

    string yask_get_version_string() {

        // Version should be in a form that will
        // allow proper sorting for numbers up to 99
        // after the major version.
        string ver = version;

        if (is_alpha)
            ver += "_alpha";
        return ver;
    }
}
