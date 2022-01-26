@ok
<?php
require_once 'kphp_tester_include.php';


class A {
    function method() { echo "A method\n"; return 1; }
}
class B {
    function method() { echo "B method\n"; return 2; }
}
class C {
    function method() { echo "C method\n"; return 3; }
}



/**
 * @kphp-template TElem
 * @kphp-param TElem[] $arr
 */
function acceptsArr($arr) {
    foreach ($arr as $o)
        if($o) $o->method();
}

/**
 * @kphp-template E1
 * @kphp-param E1 $obj
 */
function callArr1($obj) {
    acceptsArr([$obj]);
}

/**
 * @kphp-template E2
 * @kphp-param E2 $obj
 */
function callArr2($obj) {
    acceptsArr/*<E2>*/([$obj]);
}

callArr1(new A);
callArr1(new B);
callArr2(new A);
callArr2/*<B>*/(null);
callArr2(new C);



/**
 * @kphp-template T1, T2
 * @kphp-param T1[] $arr1
 * @kphp-param T2[] $arr2
 */
function f1($arr1, $arr2) {
    foreach ($arr1 as $a1)
        if ($a1) $a1->method();
    foreach ($arr2 as $a2)
        if ($a2) $a2->method();
}

f1/*<A, A>*/([null], [new A]);
f1/*<B, B>*/([new B], [new B]);
f1/*<A, B>*/([new A], [new B]);


/**
 * @kphp-template T1
 * @kphp-param T1 $first
 * @kphp-param T1[] $all
 */
function f2($first, $all) {
    if (count($all)) {
        /** @var tuple(int, T1) $f */
        $f = tuple(1, [[$all]][0][0][0]);
        if(0) $f[1]->method();
    }
    foreach ($all as $a)
        if($a) $a->method();
}

f2(new A, []);
f2(new A, [null]);


class F3c {
/**
 * @kphp-template T
 * @kphp-param T $arg
 * @kphp-param callable(T):T $cb
 */
function f3($arg, $cb) {
    $m = $cb($arg);
    $m->method();
}
}

$f3 = new F3c;
$f3->f3(new A, fn($a) => $a);
$f3->f3(new B, function($b) { $b->method(); return $b; });

/**
 * @kphp-template T : callable
 * @kphp-param T $cb
 */
function f4($cb) {
    echo $cb(), "\n";
}

f4(fn() => 4);
f4(fn() => 's');

/**
 * @kphp-template T1 : callable, T2 : callable
 * @kphp-param T1 $cb1
 * @kphp-param T2 $cb2
 */
function f5($cb1, $cb2) {
    echo $cb1(), $cb2(), "\n";
}

f5(fn() => 5, fn() => 's5');


/**
 * @kphp-template T2 : callable
 * @kphp-param T2 $cb2
 */
function f6(callable $cb1, $cb2) {
    echo $cb1(), $cb2(), "\n";
}

f6(fn() => 6, fn() => 's6');


/**
 * @kphp-template T
 * @kphp-param tuple(int, T) $t
 */
function f7($t) {
    echo $t[0], ' ', $t[1]->method(), "\n";
}

f7/*<A>*/(tuple(1, new A));


/**
 * @kphp-template T
 * @kphp-param T $first
 * @kphp-param T ...$rest
 */
function f8($first, ...$rest) {
    $first->method();
    foreach ($rest as $o)
        $o->method();
}

f8(new A, new A, new A);
f8(new B, new B);
f8(new C);


class F9c {
/**
 * @kphp-template T
 * @kphp-param T[] $all
 */
static function f9($all) {
    foreach ($all as $o)
        $o->method();
}
}

F9c::f9([new A, new A, new A]);
F9c::f9([new B]);
F9c::f9/*<C>*/([]);


class F10 {
/**
 * @kphp-template T
 * @kphp-param T ...$all
 */
function f10(...$all) {
    foreach ($all as $o)
        $o->method();
}
}

$f10 = new F10;
(function() use($f10) {
    $f10->f10(new A, new A, new A);
    $f10->f10(new B);
    $f10->f10/*<C>*/();
})();


/**
 * @kphp-template T, TCb
 * @kphp-param T   $arg
 * @kphp-param TCb $cb
 */
function strangeCall($arg, $cb) {
    $cb($arg);
}

strangeCall/*<int, callable(int):void>*/(1, function($i) { echo $i, "\n"; });
strangeCall/*<A, callable(A):string>*/(new A, function($a) { $a->method(); return 's'; });
