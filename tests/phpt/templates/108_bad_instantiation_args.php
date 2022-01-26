@kphp_should_fail
/Mismatch generics instantiation count: waiting 1, got 2/
<?php

/**
 * @kphp-template T
 * @kphp-param T $arg
 */
function f($arg) {}

f/*<int, string>*/(1);


