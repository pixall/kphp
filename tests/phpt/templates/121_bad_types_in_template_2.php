@kphp_should_fail
/pass mixed\[\] to argument \$rest of f<int>/
<?php

/**
 * @kphp-template T
 * @kphp-param T $first
 * @kphp-param T ...$rest
 */
function f($first, ...$rest) {
}

f/*<int>*/(1, 1, 2, 2, [1]);
