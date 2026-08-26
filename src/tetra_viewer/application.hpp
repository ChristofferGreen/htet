#pragma once

namespace tetra_viewer {

enum class ApplicationMode { research_viewer,world };

int run_application(int argc,char** argv,ApplicationMode mode);

}  // namespace tetra_viewer
