#include <Rigel/Asset/Assets.h>
#include <Rigel/Render/TemporalJitter.h>
#include <Rigel/Util/Yaml.h>
#include <Rigel/Voxel/WorldView.h>
#include <Rigel/input/InputState.h>

int main() {
    Rigel::Render::TemporalJitterSequence jitter;
    (void)jitter.next(1280, 720, 1.0f);
    return 0;
}
