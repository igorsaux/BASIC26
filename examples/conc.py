# Copyright (C) 2026 Igor Spichkin
# SPDX-License-Identifier: MPL-2.0

"""
conc.py - Multiprocessing baseline for the BASIC26 VM benchmark.

Mirrors the logic of conc.c: NUM_WORKERS processes pull tasks from
a shared pool. Each task is a range [lo, hi]; every number is tested for
primality via trial division. Results are collected as they complete.

This serves as a native-code baseline to measure the overhead introduced
by the BASIC26 interpreter and its preemption mechanism.
"""

import multiprocessing as mp
import time

N_TASKS     = 50
RANGE_MAX   = 20_000_000
NUM_WORKERS = 8


def is_prime(n: int) -> bool:
    if n < 2:
        return False
    if n < 4:
        return True
    if n % 2 == 0 or n % 3 == 0:
        return False
    i = 5
    while i * i <= n:
        if n % i == 0 or n % (i + 2) == 0:
            return False
        i += 6
    return True


def count_primes(args: tuple[int, int, int]) -> tuple[int, int, int, int]:
    task_id, lo, hi = args
    count = sum(1 for n in range(lo, hi + 1) if is_prime(n))
    return task_id, lo, hi, count


def main() -> None:
    chunk = (RANGE_MAX - 2) // N_TASKS
    tasks = [
        (
            i,
            2 + i * chunk,
            RANGE_MAX if i == N_TASKS - 1 else 2 + (i + 1) * chunk - 1,
        )
        for i in range(N_TASKS)
    ]

    completed = 0
    total_primes = 0

    with mp.Pool(processes=NUM_WORKERS) as pool:
        for task_id, lo, hi, count in pool.imap_unordered(count_primes, tasks):
            completed += 1
            total_primes += count
            # print(
            #     f"[pool] task {task_id:2d} done  "
            #     f"primes[{lo}..{hi}] = {count}  "
            #     f"({completed}/{N_TASKS})"
            # )

    print("\n--- Results ---")
    print(f"pi({RANGE_MAX}) = {total_primes}")
    print(f"workers = {NUM_WORKERS}, tasks = {N_TASKS}")


if __name__ == "__main__":
    main()
