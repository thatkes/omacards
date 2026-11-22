// Template for bin/probe. Copy this file outside the tracked tree, keep its
// own main(), and pass the copy to `bin/probe <copy.cpp>`.
#include <QGuiApplication>

#include <cstdio>

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    // Print with stdio: Qt message handlers are silent in offscreen runs.
    std::printf("Probe ready\n");
    return 0;
}
