@ok
<?php
require_once 'kphp_tester_include.php';

class S {
    function __toString() { return "S"; }
}

/**
 * @kphp-template TArray
 * @kphp-param TArray $arr
 */
function f($arr) {
    /** @var tuple(int, TArray) */
    $tup = tuple(0, $arr);
    foreach ($tup[1] as $i)
        echo $i, "\n";
}

/**
 * @kphp-template T
 * @kphp-param T $first
 * @kphp-param T $second
 */
function g($first, $second) {
    f/*<T[]>*/([$first, $second]);
}

g/*<int>*/(1, 2);
g/*<string>*/('a', 'b');
g/*<S>*/(new S, new S);


class User {
    public int $age;

    function __construct(int $age) { $this->age = $age; }

    function isOlder(User $r) { return $this->age > $r->age; }
}


/**
 * @kphp-template T
 * @kphp-param callable(T,T): bool $gt
 * @kphp-param T ...$arr
 * @kphp-return T
 */
function maxBy($gt, ...$arr) {
    $max = array_first_value($arr);
    for ($i = 1; $i < count($arr); ++$i) {
        if ($gt($arr[$i], $max))
            $max = $arr[$i];
    }
    return $max;
}

echo maxBy/*<int>*/(fn ($a, $b) => $a > $b, 1, 2, 9, 3), "\n";
echo maxBy/*<string>*/(fn ($a, $b) => ord($a) > ord($b), 'a', 'z', 'd'), "\n";
echo maxBy/*<User>*/(fn ($a, $b) => $a->isOlder($b), new User(8), new User(10), new User(7))->age, "\n";


