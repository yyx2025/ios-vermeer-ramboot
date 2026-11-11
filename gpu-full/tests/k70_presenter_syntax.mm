#include "../AdrenoMetal/GpuScanoutClient.h"
void k70_presenter_syntax(id<MTLDevice> device,id<MTLLibrary> library,
                         gpupresent::Scanout scanout,NGPScaleSwap swap,
                         gpuscanout::Transport transport) {
    auto *presenter=new gpupresent::Presenter(device,library,scanout);
    (void)presenter->present(swap);
    delete presenter;
    gpuscanout::Client client;
    if(client.open(device,library,transport))(void)client.serveOne();
}
