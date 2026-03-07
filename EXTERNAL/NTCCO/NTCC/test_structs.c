struct Point {
    int x;
    int y;
};

int main() {
    struct Point p;
    p.x = 5;
    p.y = 7;
    if (p.x + p.y == 12) return 0;
    return 1;
}
