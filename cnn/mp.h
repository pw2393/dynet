#pragma once
#include "cnn/cnn.h"
#include "cnn/training.h"
#include "cnn/expr.h"
#include "cnn/dict.h"
#include "cnn/lstm.h"
#include <boost/archive/text_oarchive.hpp>
#include <boost/archive/text_iarchive.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/interprocess/ipc/message_queue.hpp>
#include <boost/interprocess/shared_memory_object.hpp>
#include <boost/interprocess/mapped_region.hpp>
#include <boost/interprocess/sync/interprocess_semaphore.hpp>
#include <boost/interprocess/anonymous_shared_memory.hpp>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/shm.h>
#include <iostream>
#include <limits>
#include <fstream>
#include <vector>
#include <utility>
#include <sstream>
#include <random>

namespace cnn {
  namespace mp {
    // TODO: Pass these around instead of having them be global
    extern std::string queue_name;
    extern std::string shared_memory_name;
    extern timespec start_time;
    extern bool stop_requested;

    struct WorkloadHeader {
      bool is_dev_set;
      bool end_of_epoch;
      unsigned report_frequency;
    };

    // A simple struct to hold information about a child process
    // TODO: Rename me!
    struct Workload {
      pid_t pid;
      int c2p[2]; // Child to parent pipe
      int p2c[2]; // Parent to child pipe
    };

    // This interface is used by the child processes and called
    // once per datum.
    template<class D>
    class ILearner {
      public:
        virtual ~ILearner() {}
        virtual cnn::real LearnFromDatum(const D& datum, bool learn) = 0;
        virtual void SaveModel() = 0;
    };

    struct SharedObject {
      SharedObject() : update_mutex(1), counter_mutex(1), counter(0) {} 
      boost::interprocess::interprocess_semaphore update_mutex;
      boost::interprocess::interprocess_semaphore counter_mutex;
      unsigned counter;
    };
    extern SharedObject* shared_object;

    /// XXX: We never delete these objects
    template <class T>
    T* GetSharedMemory() {
      /*std::cerr << "Creating shared memory named " << shared_memory_name << std::endl;
      auto shm = new boost::interprocess::shared_memory_object(boost::interprocess::create_only, shared_memory_name.c_str(), boost::interprocess::read_write);
      shm->truncate(sizeof(T));
      auto region = new boost::interprocess::mapped_region (*shm, boost::interprocess::read_write);*/
      auto region = new boost::interprocess::mapped_region(boost::interprocess::anonymous_shared_memory(sizeof(T)));
      void* addr = region->get_address();
      T* obj = new (addr) SharedObject();
      return obj;
    }

    // Some simple functions that do IO to/from pipes.
    // These are used to send data from child processes
    // to the parent process or vice/versa.
    template <class T>
    T Read(int pipe) {
      T v;
      int err = read(pipe, &v, sizeof(T));
      assert (err != -1);
      return v;
    }

    template <class T>
    void Write(int pipe, const T& v) {
      int err = write(pipe, &v, sizeof(T));
      assert (err != -1);
    }

    std::string GenerateQueueName();
    std::string GenerateSharedMemoryName();

    cnn::real SumValues(const std::vector<cnn::real>& values);
    cnn::real Mean(const std::vector<cnn::real>& values);

    std::string ElapsedTimeString(const timespec& start, const timespec& end);

    unsigned SpawnChildren(std::vector<Workload>& workloads);
    std::vector<Workload> CreateWorkloads(unsigned num_children);

    // Called by the parent to process a chunk of data
    cnn::real RunDataSet(std::vector<unsigned>::iterator begin, std::vector<unsigned>::iterator end, const std::vector<Workload>& workloads,
        boost::interprocess::message_queue& mq, const WorkloadHeader& header);

    template<class D>
    void RunParent(const std::vector<D>& train_data, const std::vector<D>& dev_data, ILearner<D>* learner,
       std::vector<Workload>& workloads, unsigned num_iterations, unsigned dev_frequency, unsigned report_frequency) {
      const unsigned num_children = workloads.size();
      boost::interprocess::message_queue mq(boost::interprocess::open_or_create, queue_name.c_str(), 10000, sizeof(unsigned));
      std::vector<unsigned> train_indices(train_data.size());
      std::iota(train_indices.begin(), train_indices.end(), 0);

      std::vector<unsigned> dev_indices(dev_data.size());
      std::iota(dev_indices.begin(), dev_indices.end(), 0);

      cnn::real best_dev_loss = std::numeric_limits<cnn::real>::max();
      for (unsigned iter = 0; iter < num_iterations && !stop_requested; ++iter) {
        // Shuffle the training data indices
        random_shuffle(train_indices.begin(), train_indices.end());

        cnn::real train_loss = 0.0;

        std::vector<unsigned>::iterator begin = train_indices.begin();
        while (begin != train_indices.end()) {
          std::vector<unsigned>::iterator end = begin + dev_frequency;
          if (end > train_indices.end()) {
            end = train_indices.end();
          }
          double fractional_iter = iter + 1.0 * distance(train_indices.begin(), end) / train_indices.size();
          train_loss += RunDataSet(begin, end, workloads, mq, {false, end == train_indices.end(), report_frequency});
          std::cerr << fractional_iter << "\t" << "loss = " << train_loss << std::endl;

          if (stop_requested) {
            break;
          }

          cnn::real dev_loss = RunDataSet(dev_indices.begin(), dev_indices.end(), workloads, mq, {true, false, report_frequency});
          bool new_best = (dev_loss < best_dev_loss);
          std::cerr << fractional_iter << "\t" << "dev loss = " << dev_loss << (new_best ? " (New best!)" : "") << std::endl;
          if (stop_requested) {
            break;
          }
          if (new_best) {
            learner->SaveModel();
            best_dev_loss = dev_loss;
          }

          begin = end;
        }
      }

      // Kill all children one by one and wait for them to exit
      for (unsigned cid = 0; cid < num_children; ++cid) {
        bool cont = false;
        Write(workloads[cid].p2c[1], &cont);
        wait(NULL);
      }
    }

    template <class D>
    int RunChild(unsigned cid, ILearner<D>* learner, Trainer* trainer,
        std::vector<Workload>& workloads, const std::vector<D>& train_data,
        const std::vector<D>& dev_data) {
      const unsigned num_children = workloads.size();
      assert (cid >= 0 && cid < num_children);
      unsigned i;
      unsigned priority;
      boost::interprocess::message_queue::size_type recvd_size;
      boost::interprocess::message_queue mq(boost::interprocess::open_or_create, queue_name.c_str(), 10000, sizeof(unsigned));
      while (true) {
        // Check if the parent wants us to exit
        bool cont = Read<bool>(workloads[cid].p2c[0]); 
        if (!cont) {
          break;
        }

        // Check if we're running on the training data or the dev data
        //std::cerr << "#" << cid << " waiting for header" << std::endl;
        WorkloadHeader header = Read<WorkloadHeader>(workloads[cid].p2c[0]); 

        // Run the actual training loop
        cnn::real total_loss = 0;
        cnn::real batch_loss = 0;
        unsigned batch_counter = 0;
        while (true) {
          mq.receive(&i, sizeof(unsigned), recvd_size, priority);
          if (i == -1U) {
            break;
          }

          assert (i < (header.is_dev_set ? dev_data.size() : train_data.size()));
          const D& datum = (header.is_dev_set ? dev_data[i] : train_data[i]);
          cnn::real datum_loss = learner->LearnFromDatum(datum, !header.is_dev_set);
          total_loss += datum_loss;
          batch_loss += datum_loss;
          batch_counter++;

          bool do_update = !header.is_dev_set && cid == 0 && (batch_counter % 10 == 9);
          shared_object->counter_mutex.wait();
          unsigned counter = ++shared_object->counter;
          if (do_update) { shared_object->counter = 0; }
          shared_object->counter_mutex.post();
          if (do_update) {
            /*std::cerr << "Parameter checking part 1..." << std::endl;
            for (LookupParameters* p : trainer->model->lookup_parameters_list()) {
              for (unsigned s = 0; s < p->size(); ++s) {
                assert (p->values[s].is_valid());
                assert (p->grads[s].is_valid());
              }
            }*/
            shared_object->update_mutex.wait();
            trainer->update(1.0 / counter); 
            shared_object->update_mutex.post();
            /*std::cerr << "Parameter checking part 2..." << std::endl;
            for (LookupParameters* p : trainer->model->lookup_parameters_list()) {
              for (unsigned s = 0; s < p->size(); ++s) {
                assert (p->values[s].is_valid());
                assert (p->grads[s].is_valid());
              }
            }
            std::cerr << "Parameter checking done." << std::endl;*/
          }
          if (batch_counter == header.report_frequency) {
            if (cid == 0) {
              std::cerr << (header.is_dev_set ? "dev" : "train") << " x-ent: " << batch_loss / batch_counter << std::endl;
            }
            batch_loss = 0;
            batch_counter = 0;
          }
        }
        if (header.end_of_epoch) {
          trainer->update_epoch();
        }

        // Let the parent know that we're done and return the loss value
        Write(workloads[cid].c2p[1], total_loss);
      }
      return 0;
    }

    template<class D>
    void RunMultiProcess(unsigned num_children, ILearner<D>* learner, Trainer* trainer, const std::vector<D>& train_data,
        const std::vector<D>& dev_data, unsigned num_iterations, unsigned dev_frequency, unsigned report_frequency) {
      assert (cnn::ps->is_shared());
      queue_name = GenerateQueueName();
      boost::interprocess::message_queue::remove(queue_name.c_str());
      boost::interprocess::message_queue::remove(queue_name.c_str());
      shared_memory_name = GenerateSharedMemoryName();
      shared_object = GetSharedMemory<SharedObject>();
      std::vector<Workload> workloads = CreateWorkloads(num_children);
      unsigned cid = SpawnChildren(workloads);
      if (cid < num_children) {
        RunChild(cid, learner, trainer, workloads, train_data, dev_data);
      }
      else {
        RunParent(train_data, dev_data, learner, workloads, num_iterations, dev_frequency, report_frequency);
      }
    }
  }
}
