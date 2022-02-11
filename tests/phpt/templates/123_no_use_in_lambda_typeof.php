@kphp_should_fail
/typeof\(\) can be used for instances only/
<?php

class BB {
    function method() { echo "B method\n"; return 1; }
}
class D1 extends BB {
    function dMethod() { echo "d1\n"; }
}

/**
 * @kphp-template T1, T2
 * @kphp-param T1 $o1
 * @kphp-param T2 $o2
 * @kphp-return ?T1
 */
function castO2ToTypeofO1($o1, $o2) {
   /** @var T1 */
   $casted = null;
   // an error is missing use()
   (function() use($casted, $o2) {
        $casted = instance_cast($o2, typeof($o1));
   })();
   echo get_class($casted), "\n";
   return $casted;
}

/** @var BB */
$bb = new D1;
castO2ToTypeofO1($bb, new D1);
