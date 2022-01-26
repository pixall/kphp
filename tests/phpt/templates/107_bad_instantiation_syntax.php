@kphp_should_fail
/Apply phpdocs/
/tuple\(/
/Could not parse generics instantiation/
<?php

/**
 * @kphp-template T
 * @kphp-param T $arg
 */
function f($arg) {}

f/*<tuple(>*/();

