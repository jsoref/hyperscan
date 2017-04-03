/*
 * Copyright (c) 2015-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of Intel Corporation nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/** \file
 * \brief Hamster Wheel Literal Matcher: build code.
 */

#include "hwlm_build.h"

#include "grey.h"
#include "hwlm.h"
#include "hwlm_internal.h"
#include "hwlm_literal.h"
#include "noodle_engine.h"
#include "noodle_build.h"
#include "scratch.h"
#include "ue2common.h"
#include "fdr/fdr_compile.h"
#include "util/compile_context.h"
#include "util/compile_error.h"
#include "util/ue2string.h"

#include <cassert>
#include <cstring>
#include <vector>

using namespace std;

namespace ue2 {

static
void dumpLits(UNUSED const vector<hwlmLiteral> &lits) {
#ifdef DEBUG
    DEBUG_PRINTF("building lit table for:\n");
    for (const auto &lit : lits) {
        printf("\t%u:%016llx %s%s\n", lit.id, lit.groups,
               escapeString(lit.s).c_str(), lit.nocase ? " (nc)" : "");
    }
#endif
}

#ifndef NDEBUG
// Called by an assertion.
static
bool everyoneHasGroups(const vector<hwlmLiteral> &lits) {
    for (const auto &lit : lits) {
        if (!lit.groups) {
            return false;
        }
    }
    return true;
}
#endif

static
bool isNoodleable(const vector<hwlmLiteral> &lits,
                  const CompileContext &cc) {
    if (!cc.grey.allowNoodle) {
        return false;
    }

    if (lits.size() != 1) {
        DEBUG_PRINTF("too many literals for noodle\n");
        return false;
    }

    if (!lits.front().msk.empty()) {
        DEBUG_PRINTF("noodle can't handle supplementary masks\n");
        return false;
    }

    return true;
}

bytecode_ptr<HWLM> hwlmBuild(const vector<hwlmLiteral> &lits, bool make_small,
                             const CompileContext &cc,
                             UNUSED hwlm_group_t expected_groups) {
    assert(!lits.empty());
    dumpLits(lits);

    // Check that we haven't exceeded the maximum number of literals.
    if (lits.size() > cc.grey.limitLiteralCount) {
        throw ResourceLimitError();
    }

    // Safety and resource limit checks.
    u64a total_chars = 0;
    for (const auto &lit : lits) {
        assert(!lit.s.empty());

        if (lit.s.length() > cc.grey.limitLiteralLength) {
            throw ResourceLimitError();
        }
        total_chars += lit.s.length();
        if (total_chars > cc.grey.limitLiteralMatcherChars) {
            throw ResourceLimitError();
        }

        // We do not allow the all-ones ID, as we reserve that for internal use
        // within literal matchers.
        if (lit.id == 0xffffffffu) {
            assert(!"reserved id 0xffffffff used");
            throw CompileError("Internal error.");
        }
    }

    u8 engType = 0;
    size_t engSize = 0;
    shared_ptr<void> eng;

    DEBUG_PRINTF("building table with %zu strings\n", lits.size());

    assert(everyoneHasGroups(lits));

    if (isNoodleable(lits, cc)) {
        DEBUG_PRINTF("build noodle table\n");
        engType = HWLM_ENGINE_NOOD;
        const hwlmLiteral &lit = lits.front();
        auto noodle = noodBuildTable(lit);
        if (noodle) {
            engSize = noodle.size();
        }
        eng = move(noodle);
    } else {
        DEBUG_PRINTF("building a new deal\n");
        engType = HWLM_ENGINE_FDR;
        auto fdr = fdrBuildTable(lits, make_small, cc.target_info, cc.grey);
        if (fdr) {
            engSize = fdr.size();
        }
        eng = move(fdr);
    }

    if (!eng) {
        return nullptr;
    }

    assert(engSize);
    if (engSize > cc.grey.limitLiteralMatcherSize) {
        throw ResourceLimitError();
    }

    auto h = make_bytecode_ptr<HWLM>(ROUNDUP_CL(sizeof(HWLM)) + engSize, 64);

    h->type = engType;
    memcpy(HWLM_DATA(h.get()), eng.get(), engSize);

    return h;
}

size_t hwlmSize(const HWLM *h) {
    size_t engSize = 0;

    switch (h->type) {
    case HWLM_ENGINE_NOOD:
        engSize = noodSize((const noodTable *)HWLM_C_DATA(h));
        break;
    case HWLM_ENGINE_FDR:
        engSize = fdrSize((const FDR *)HWLM_C_DATA(h));
        break;
    }

    if (!engSize) {
        return 0;
    }

    return engSize + ROUNDUP_CL(sizeof(*h));
}

size_t hwlmFloodProneSuffixLen(size_t numLiterals, const CompileContext &cc) {
    const size_t NO_LIMIT = ~(size_t)0;

    // NOTE: this function contains a number of magic numbers which are
    // conservative estimates of flood-proneness based on internal details of
    // the various literal engines that fall under the HWLM aegis. If you
    // change those engines, you might need to change this function too.

    DEBUG_PRINTF("%zu literals\n", numLiterals);

    if (cc.grey.allowNoodle && numLiterals <= 1) {
        DEBUG_PRINTF("noodle\n");
        return NO_LIMIT;
    }

    if (cc.grey.fdrAllowTeddy) {
        if (numLiterals <= 48) {
            DEBUG_PRINTF("teddy\n");
            return 3;
        }
        if (cc.target_info.has_avx2() && numLiterals <= 96) {
            DEBUG_PRINTF("avx2 teddy\n");
            return 3;
        }
    }

    // TODO: we had thought we could push this value up to 9, but it seems that
    // hurts performance on floods in some FDR models. Super-conservative for
    // now.
    DEBUG_PRINTF("fdr\n");
    return 3;
}

} // namespace ue2
