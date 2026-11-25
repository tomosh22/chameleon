#pragma once

// Fixture `legacy_lambdas` (WP-46): three lambdas with a static counter each, in one translation unit built with cl's legacy
// lambda processor (closures named by a hash).

int ll_a();     // 100000 + calls of the first lambda
int ll_b();     // 200000 + calls of the second lambda (times 2 after edit body_2)
int ll_c();     // 300000 + calls of the third lambda
int ll_mark();  // 1; 2 after edit mark_1
int ll_made();  // counters constructed: stays 3
