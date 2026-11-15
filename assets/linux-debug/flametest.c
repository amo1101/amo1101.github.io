// flametest.c — synthetic CPU-bound workload for flame graph validation
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARRAY_SIZE 20000

// --- Tower 1: recursive fibonacci (shows recursion as stacked identical frames) ---
long fib(int n) {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

void run_fib_tower(void) {
    for (int i = 0; i < 32; i++) {
        fib(28);   // deep enough to show real recursion depth, cheap enough to loop
    }
}

// --- Tower 2: prime sieve (nested calls, no recursion) ---
int is_prime(long n) {
    if (n < 2) return 0;
    for (long i = 2; i * i <= n; i++) {
        if (n % i == 0) return 0;
    }
    return 1;
}

long count_primes_up_to(long limit) {
    long count = 0;
    for (long i = 2; i < limit; i++) {
        if (is_prime(i)) count++;
    }
    return count;
}

void run_prime_tower(void) {
    for (int i = 0; i < 3; i++) {
        count_primes_up_to(300000);
    }
}

// --- Tower 3: bubble sort (deliberately O(n^2), distinct call depth) ---
void swap_elems(int *a, int *b) {
    int tmp = *a;
    *a = *b;
    *b = tmp;
}

void bubble_sort(int *arr, int n) {
    for (int i = 0; i < n - 1; i++) {
        for (int j = 0; j < n - i - 1; j++) {
            if (arr[j] > arr[j + 1]) {
                swap_elems(&arr[j], &arr[j + 1]);
            }
        }
    }
}

void run_sort_tower(void) {
    int *arr = malloc(ARRAY_SIZE * sizeof(int));
    for (int i = 0; i < ARRAY_SIZE; i++) {
        arr[i] = ARRAY_SIZE - i;   // reverse-sorted: worst case for bubble sort
    }
    bubble_sort(arr, ARRAY_SIZE);
    free(arr);
}

int main(void) {
    printf("Starting workload (pid %d)...\n", getpid());

    run_fib_tower();
    run_prime_tower();
    run_sort_tower();

    printf("Done.\n");
    return 0;
}