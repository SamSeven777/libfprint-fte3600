/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Driver integration build policy. The reusable matcher does not include this.
 */
#pragma once

#ifndef FTE3600_ENABLE_PERSONAL_AUTH
#define FTE3600_ENABLE_PERSONAL_AUTH 0
#endif

#if FTE3600_ENABLE_PERSONAL_AUTH != 0 && FTE3600_ENABLE_PERSONAL_AUTH != 1
#error "FTE3600_ENABLE_PERSONAL_AUTH must be zero or one"
#endif
