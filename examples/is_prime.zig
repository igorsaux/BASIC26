pub export fn is_prime(n: u64) u64 {
    if (n < 2) {
        return 0;
    }

    if (n < 4) {
        return 1;
    }

    if (n % 2 == 0) {
        return 0;
    }

    if (n % 3 == 0) {
        return 0;
    }

    var i: u64 = 5;

    while (i * i <= n) {
        if (n % i == 0) {
            return 0;
        }

        if (n % (i + 2) == 0) {
            return 0;
        }

        i += 6;
    }

    return 1;
}
