#include <taskflow/taskflow.hpp>  // the only include you need
#include <fstream>

std::array<std::atomic<size_t>, 10> histogram;

void read_file(const char* filename, std::vector<int>& digits){
  std::ifstream ifs(filename);
  char digit;
  while(ifs >> digit){
    digits.push_back(digit - '0');
  }
}

void count_digits(const char* filename, std::vector<int>& digits, tf::SubflowBuilder& subflow){
  read_file(filename, digits);
  if(digits.size() < 1000000u){
    for(const auto& d : digits){
      histogram[d] ++;
    }
  }
  else{
    // Spawn new tasks to parallelize counting every 1000000 digits
    for(size_t i=0; i<digits.size(); i+=1000000u){
      subflow.silent_emplace(
        [beg=i, end=std::min(i+1000000u, digits.size()), &digits](){
          for(size_t i=beg; i<end ;i++){
            histogram[digits[i]] ++;
          }
        }
      ).name("subtask" + std::to_string(i/1000000u));
    }
  }
}
  

int main(){

  const auto file1 = "file1";
  const auto file2 = "file2";
  const auto file3 = "file3";

  std::vector<int> buffer1;
  std::vector<int> buffer2;
  std::vector<int> buffer3;

  // Reset the histogram
  for(auto &h : histogram) {
    h = 0;
  }

  tf::Taskflow tf(std::thread::hardware_concurrency());

  // Spawn three tasks to parse the three files
  auto [A, B, C] = tf.silent_emplace(   
    [&] (auto &subflow) { count_digits(file1, buffer1, subflow); },   
    [&] (auto &subflow) { count_digits(file2, buffer2, subflow); },   
    [&] (auto &subflow) { count_digits(file3, buffer3, subflow); }   
  );
  
  A.name(file1);  
  B.name(file2);  
  C.name(file3);  
   
  // dispatch the graph without cleanning up topologies  
  tf.dispatch().get();

  // dump the entire graph (including dynamic tasks)
  std::cout << tf.dump_topologies();

  return 0;
}
