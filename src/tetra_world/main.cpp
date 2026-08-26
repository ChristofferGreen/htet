#include "tetra_viewer/application.hpp"

int main(int argc,char** argv) {
  return tetra_viewer::run_application(argc,argv,tetra_viewer::ApplicationMode::world);
}
