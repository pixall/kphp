@kphp_should_fail
/Duplicate generics <T> in declaration/
<?php

/**
 * @kphp-template T, T
 * @kphp-param T $arg
 */
function f($arg) {}

f();

