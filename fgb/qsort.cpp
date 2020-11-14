#include <algorithm>

extern "C" {

  void cqsort( int*f, int*l ){
    std::sort(f,l);
  }
  
}
