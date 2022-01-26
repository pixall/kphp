@kphp_should_fail
/pass string to argument \$a of f<int>/
<?php

/**
 * @kphp-template T
 * @kphp-param T $a
 */
function f($a) {}

f/*<int>*/('5');

