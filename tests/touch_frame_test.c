#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../xnu/vermeer/vermeer_touch_frame.h"
int main(void)
{
    struct vth_state state={0};struct vt_frame frame={0};struct vth_plan p;
    assert(vth_prepare(&state,&frame,&p)==0 && !p.count);
    /* A swipe starts at the physical bottom edge, then moves toward its center. */
    frame.point[0]=(struct vt_point){720,3180,1};
    assert(vth_prepare(&state,&frame,&p)==1 && p.count==1 && p.any);
    assert(p.mask[0]==(VTH_TOUCH|VTH_POSITION|VTH_IDENTITY|VTH_EDGE));
    assert(p.next.point[0].edge && p.next.point[0].y<65536);
    /* Simulated allocation failure: repeat the same uncommitted report. */
    struct vth_plan retry;assert(vth_prepare(&state,&frame,&retry)==1);
    assert(!memcmp(&p,&retry,sizeof(p)));state=p.next;
    assert(vth_prepare(&state,&frame,&p)==0);
    frame.point[0].y=1800;
    assert(vth_prepare(&state,&frame,&p)==1 && p.mask[0]==(VTH_POSITION|VTH_EDGE));state=p.next;
    frame.point[0].active=0;
    assert(vth_prepare(&state,&frame,&p)==1 && p.mask[0]==(VTH_TOUCH|VTH_EDGE) && !p.any);state=p.next;
    assert(vth_prepare(&state,&frame,&p)==0);
    /* Reused IDs get new origin classification. */
    frame.point[0]=(struct vt_point){720,1600,1};
    frame.point[3]=(struct vt_point){721,1601,1};
    assert(vth_prepare(&state,&frame,&p)==1 && p.count==2 && !p.next.point[0].edge);state=p.next;
    frame.point[0].x=0;frame.point[3].active=0;
    assert(vth_prepare(&state,&frame,&p)==1 && p.count==2 && p.any);
    assert(p.mask[0]==VTH_POSITION && p.mask[3]==VTH_TOUCH && !p.next.point[0].edge);
    struct vth_plan saved=p;frame.point[1]=(struct vt_point){1440,2,1};
    assert(vth_prepare(&state,&frame,&p)==-1 && !memcmp(&saved,&p,sizeof(p)));
    frame.point[1]=(struct vt_point){2,3200,1};assert(vth_prepare(&state,&frame,&p)==-1);
    frame.point[1]=(struct vt_point){2,2,2};assert(vth_prepare(&state,&frame,&p)==-1);
    puts("Atomic touch frames, full-panel edge gestures, release and retry: PASS");
}
