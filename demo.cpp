#include "fasthumb.hpp"

int main(int argc, char *argv[]) {
    Fasthumb thumb(240, 192, 10);

    thumb.extractKeyFrames(argv[1], atoi(argv[2]));
    thumb.initializeDecoder();
    thumb.decodeFrames();
}

