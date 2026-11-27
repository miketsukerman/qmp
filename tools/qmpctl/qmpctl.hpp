// SPDX-License-Identifier: Apache-2.0
// qmpctl — inspect and exercise running qmp services from the command line. Links only
// the C ABI (ADR-006) and is driven only by reflection data: it contains no generated
// code and works with any service whose runtime answers QMP_OP_REFLECT (or for which a
// <svc>.reflect.json is passed with --reflect).
#ifndef QMPCTL_QMPCTL_HPP
#define QMPCTL_QMPCTL_HPP

#include <iosfwd>
#include <string>
#include <vector>

namespace qmpctl {

// Run one command line (args excludes argv[0]). Returns the process exit code:
// 0 success, 1 the command failed (transport error, application status, bad reply),
// 2 usage error. `in` feeds `call ... -`.
int run(const std::vector<std::string> &args, std::istream &in, std::ostream &out,
        std::ostream &err);

// Ask a running `subscribe` to stop after the current poll slice. Async-signal-safe.
void request_stop();

} // namespace qmpctl
#endif
