#!/usr/bin/ucode

'use strict';

let resolver = require("bgpdisco.resolver");

let query = ARGV[0];

let result = resolver.resolve(query);
printf("Result is\n%.4J\n", result);
