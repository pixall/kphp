@kphp_should_fail
/Couldn't reify generics <TElem> for call/
<?php

/**
 * @kphp-template TElem
 * @kphp-param TElem[] $arg
 */
function f($arg) {}

f([null]);


