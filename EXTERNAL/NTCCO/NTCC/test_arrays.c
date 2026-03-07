int sum(int *p, int n) {
    int i = 0;
    int acc = 0;
    while (i < n) {
        acc = acc + p[i];
        i = i + 1;
    }
    return acc;
}

int main() {
    int a[4];
    a[0] = 1;
    a[1] = 2;
    a[2] = 3;
    a[3] = 4;
    int s = sum(a, 4);
    if (s == 10) return 0;
    return 1;
}
