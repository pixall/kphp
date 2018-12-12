<?php

namespace LibClasses;

class A {
    static public $field = "static public field\n";

    static public function static_fun() {
        echo "[example1] A: called static_fun()\n";
        echo "[example1] A: call SUB\V::static_fun()\n";
        return SUB\V::static_fun();
    }

    var $value = 'str';

    public function set_value($v) {
        echo "[example1] A: called public function set_value()\n";
        $this->value = $v;
    }

    public function get_value() {
        echo "[example1] A: called public function get_value()\n";
        return $this->value;
    }
}