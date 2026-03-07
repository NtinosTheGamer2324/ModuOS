struct Point {
    int x;
    int y;
};

int main() {
    struct Point p;
    struct Point *pp = &p;
    pp->x = 5;
    pp->y = 7;
    if (pp->x + pp->y == 12) return 0;
    return 1;
}
