@ok
<?php
//require_once 'kphp_tester_include.php';

// todo polyfills
#ifndef KPHP
    function typeof($obj) {
        if (!is_object($obj)) throw new Exception("typeof() must accept an object");
        // todo comment that it's not true generally, but we can't do better in PHP
        return get_class($obj);
    }

    function instance_cast($obj, string $class_name) {
        if ($obj === null)
            return null;
        if (!($obj instanceof $class_name))
            return null;
        return $obj;
    }

    function array_reserve_from($arr, $arr2) {
    }
#endif

class B {
    function method() { echo "B method\n"; return 1; }
}
class D1 extends B {
    function dMethod() { echo "d1\n"; }
}
class D2 extends B {
    function dMethod() { echo "d2\n"; }
}

/**
 * @kphp-template T, DstClass
 * @kphp-param T $obj
 * @kphp-param class-string<DstClass> $to_classname
 * @kphp-return DstClass
 */
function my_cast($obj, $to_classname) {
    return instance_cast($obj, $to_classname);
}

/**
 * @kphp-template T, DstClass
 * @kphp-param T $obj
 * @kphp-param class-string<DstClass> $if_classname
 */
function callDMethodIfNotNull($obj, $if_classname) {
    /** @var DstClass */
    $casted = my_cast($obj, $if_classname);
    if ($casted)
        $casted->dMethod();
    else
        echo "cast to $if_classname is null: obj is ", get_class($obj), "\n";
}

/** @var B */
$b = new D1;
my_cast($b, D1::class)->dMethod();

callDMethodIfNotNull(new D1, D1::class);
callDMethodIfNotNull(new D1, D2::class);
callDMethodIfNotNull(new D2, D1::class);
callDMethodIfNotNull(new D2, D2::class);

/**
 * @kphp-template TElem, ToName
 * @kphp-param TElem[] $arr
 * @kphp-param class-string<ToName> $to
 * @kphp-return ToName[]
 */
function my_array_cast($arr, $to) {
    $out = [];
    array_reserve_from($out, $arr);
    foreach ($arr as $k => $v) {
        $out[$k] = instance_cast($v, $to);
    }
    return $out;
}

/**
 * @param B[] $arr
 */
function demoCastAndPrintAllD1(array $arr) {
    $casted_arr = my_array_cast($arr, D1::class);
    foreach ($casted_arr as $obj)
        if ($obj) $obj->dMethod();
}

demoCastAndPrintAllD1([new D1, new D2, new D1]);

/**
 * @kphp-template T1, T2
 * @kphp-param T1 $o1
 * @kphp-param T2 $o2
 * @kphp-return ?T1
 */
function castO2ToTypeofO1($o1, $o2) {
   /** @var T1 */
   $casted = instance_cast($o2, typeof($o1));
   echo get_class($casted), "\n";
   return $casted;
}

/** @var B */
$bb = new D1;
castO2ToTypeofO1($bb, new D1);


