#!/bin/bash


./bin/tuac examples/test5.tua
llvm-gcc  -o bin/test5.bin bin/output.ll
time ./bin/test5.bin
