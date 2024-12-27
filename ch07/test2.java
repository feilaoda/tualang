public class test2 {
    static int test(int n) {
        int a = 0;
        int b[] = new int[100];
        for (int i = 0; i <= n; i++) {
            a = a + i;
            b[i%100] = a;
        }
        return a;
    }
    public static void main(String[] args) {
       long s = System.currentTimeMillis();
       System.out.println(test(Integer.parseInt(args[0])));
       long e = System.currentTimeMillis();
       System.out.println("ms:"+(e-s));
    }
}