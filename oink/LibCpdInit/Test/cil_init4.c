//  typedef unsigned long longtype;

//  typedef longtype partidtype ;

//  typedef char parttype[10] ;

typedef struct Connection_Type {
//    partidtype to ;
  int to ;
//    parttype type ;
//    longtype length ;
  int length ;
} Connection ;

//  extern void printf(char *, ...);
//  #define E(n) { printf("Error %d\n", n); return(1); }

// From VORTEX
int main() {
//    static
  Connection link[3] =
  {
    {1,
//       "link1",
     11},
    {2,
//       "link2",
     22},
    {3,
//       "link3",
     33
    }
  };

//    if(sizeof(link[0]) != 4 + 10 + 2 + 4) E(1);

//    if(link[0].length != 10) E(2);

//    if(link[2].length != 30) E(3);

//    if(strcmp("link2", link[1].type)) E(4);

//    if(link[1].type[6] != 0) E(5);

//    printf("Success\n");
//    return 0;
}
