#include <chrono>
#include <iostream>

#include "kzevent/timer/timer.hpp"

int main() {
  using namespace kzevent::timer;
  using namespace kzevent::core;

  /* 创建一个loop */
  auto loop = std::make_unique<Loop>();
  loop->start();

  /* 创建一个定时器 */
  auto timer = Timer::make_timer(*loop);

  /* 定时任务的持有者A */
  auto A = std::make_shared<int>(0);

  /* 定时任务A */
  auto repeat_task = [A]() {
    ++(*A);
    std::cout << *A << std::endl;
  };

  /* 定时任务的持有者B */
  auto B = std::make_shared<int>(0);

  /* 定时任务B */
  auto once_task = [B]() {
    --(*B);
    std::cout << *B << std::endl;
  };

  /* 添加多个定时任务 */
  timer->post_repeat_task(repeat_task, A, 1000);
  timer->post_repeat_task(once_task, B, 2000);

  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  }
}