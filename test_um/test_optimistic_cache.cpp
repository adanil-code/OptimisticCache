/*
* Apache Optimistic Cache Test/Sample
* Copyright 2026 Alexander Danileiko
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at:
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* This software is provided on an "AS IS" basis, WITHOUT WARRANTIES OR CONDITIONS
* OF ANY KIND, either express or implied.
*/

#include <iostream>
#include "optimistic_cache.h"
#include "test_optimistic_cache_common.h"

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    std::cout.setf(std::ios::unitbuf);

    std::cout << "\n";
    std::cout << "=========================================================\n";
    std::cout << "            UM OPTIMISTIC CACHE TEST SUITE               \n";
    std::cout << "=========================================================\n";

    return RunAllCacheTests();
}
