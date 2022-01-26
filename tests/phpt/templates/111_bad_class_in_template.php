@kphp_should_fail
/g\/\*<AAA>\*\/\(null\)/
/Could not find class AAA/
<?php

/**
 * @kphp-template T
 * @kphp-param T $arg
 */
function g($arg) {
}

g/*<AAA>*/(null);

