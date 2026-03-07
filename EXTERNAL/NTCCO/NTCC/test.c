int add(int a, int b) {
    return a + b;
}

int main() {
    int x = 3;
    int y = 4;
    int z = add(x, y);
    if (z == 7) {
        return 0;
    }
    return 1;
}