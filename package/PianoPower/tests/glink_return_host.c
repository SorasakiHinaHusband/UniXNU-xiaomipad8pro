/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Copyright (c) 2026, Leo Zhang. All rights reserved. */
/* Production GLINK receive-intent ownership under shared TX FIFO pressure. Synthetic bytes only. */
#include "../QcomGlink.h"
#include <stdio.h>
#include <string.h>

static unsigned checks, failures, deliveries, kicks;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)
static void put32(uint8_t *p, uint32_t v) { for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i)); }
static uint32_t get32(const uint8_t *p) { return p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24; }
static void kick(void *p) {(void)p; ++kicks;}
static void receive(void *p,QcomGlinkChannel *c,const uint8_t *data,uint32_t n)
{ (void)p; (void)c; CHECK(n==4 && !memcmp(data,"lux!",4)); ++deliveries; }
typedef struct {
    volatile uint32_t th,tt,rh,rt;
    uint8_t tx[512],rx[512],storage[16*4];
    QcomGlink g;
} Fixture;
static QcomGlinkChannel *setup(Fixture *f,int reuse,unsigned intents)
{
    memset(f,0,sizeof *f);
    const QcomGlinkFifo tx={&f->th,&f->tt,f->tx,sizeof f->tx},rx={&f->rh,&f->rt,f->rx,sizeof f->rx};
    qcom_glink_init(&f->g,tx,rx,kick,NULL);
    f->g.version_sent=f->g.version_agreed=1;
    f->g.features=reuse?QCOM_GLINK_FEATURE_INTENT_REUSE:0;
    const QcomGlinkClient client={NULL,receive};
    QcomGlinkChannel *c=qcom_glink_channel(&f->g,"IPCRTR",16,intents,f->storage,&client,NULL,1);
    CHECK(c!=NULL);
    c->open_sent=c->open_acked=c->remote_open=c->up=1;c->rcid=7;
    for(unsigned i=0;i<intents;++i)c->local[i].advertised=1;
    return c;
}
static void remote(Fixture *f,uint16_t command,uint16_t p1,uint32_t p2,const uint8_t *data,uint32_t n)
{
    uint8_t header[8]={(uint8_t)command,(uint8_t)(command>>8),(uint8_t)p1,(uint8_t)(p1>>8)};
    put32(header+4,p2);
    uint32_t h=f->rh;
    for(unsigned i=0;i<8;++i)f->rx[h++%sizeof f->rx]=header[i];
    for(unsigned i=0;i<n;++i)f->rx[h++%sizeof f->rx]=data[i];
    f->rh=((h+7)&~7u)%sizeof f->rx;
}
static void data(Fixture *f,uint16_t command,uint32_t id,const char *bytes,uint32_t n,uint32_t left)
{
    uint8_t payload[24];put32(payload,n);put32(payload+4,left);memcpy(payload+8,bytes,n);
    remote(f,command,7,id,payload,8+n);
}
static unsigned take(Fixture *f,uint16_t wanted,uint32_t wanted_p2)
{
    unsigned found=0;
    while(f->tt!=f->th){
        const uint32_t t=f->tt;
        uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f->tx[(t+i)%sizeof f->tx];
        const uint16_t cmd=(uint16_t)(h[0]|h[1]<<8);
        const uint32_t p2=get32(h+4);
        if(cmd==wanted && p2==wanted_p2)++found;
        uint32_t n=8;
        if(cmd==QCOM_GLINK_INTENT)n+=8*p2;
        if(cmd==QCOM_GLINK_OPEN)n=(8+p2+7)&~7u;
        f->tt=(t+n)%sizeof f->tx;
    }
    return found;
}
static void completion(int reuse,int fragmented,unsigned intents)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,reuse,intents);
    const unsigned before=deliveries;
    /* No ordinary command fits; the physically reserved READ_NOTIF bytes still do. */
    f.th=sizeof f.tx-24;
    if(fragmented){data(&f,QCOM_GLINK_TX_DATA,1,"lu",2,2);qcom_glink_poll(&f.g);CHECK(deliveries==before);}
    data(&f,fragmented?QCOM_GLINK_TX_DATA_CONT:QCOM_GLINK_TX_DATA,1,fragmented?"x!":"lux!",fragmented?2:4,0);
    CHECK(qcom_glink_poll(&f.g)>=1 && deliveries==before+1 && f.rt==f.rh);
    /* A remote cannot re-use this intent until it sees DONE. No payload may be delivered twice on retry. */
    f.tt=f.th;
    qcom_glink_poll(&f.g);
    CHECK(take(&f,reuse?QCOM_GLINK_RX_DONE_W_REUSE:QCOM_GLINK_RX_DONE,1)==1);
    CHECK(deliveries==before+1 && c->local[0].busy==0);
    CHECK(f.g.done_deferred==1 && f.g.done_retried==1 && !c->local[0].done_due);
    CHECK(f.g.tx_blocked>0);
    qcom_glink_poll(&f.g);
    CHECK(take(&f,reuse?QCOM_GLINK_RX_DONE_W_REUSE:QCOM_GLINK_RX_DONE,1)==0 && deliveries==before+1);
    CHECK(!f.g.retry_pending);
}
static void intent_grant(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,0,1);
    /* The non-reuse DATA completion frees an intent during this poll. Its DONE and re-advertisement fit,
     * but the following grant ACK does not: replaying the request would turn grant=1 into refusal=0. */
    f.th=sizeof f.tx-48;
    data(&f,QCOM_GLINK_TX_DATA,1,"lux!",4,0);
    remote(&f,QCOM_GLINK_RX_INTENT_REQ,7,16,NULL,0);
    qcom_glink_poll(&f.g);
    CHECK(c->local[0].advertised && f.g.reply_due && f.g.reply_deferred==1 && f.rt==f.rh);
    f.tt=f.th;
    qcom_glink_poll(&f.g);
    CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,1)==1 && f.g.reply_retried==1 && !f.g.reply_due);
    qcom_glink_poll(&f.g);CHECK(take(&f,QCOM_GLINK_RX_INTENT_REQ_ACK,1)==0 && !f.g.retry_pending);
}
static void multiple_returns(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,3);
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    for(unsigned i=1;i<=3;++i)data(&f,QCOM_GLINK_TX_DATA,i,"lux!",4,0);
    qcom_glink_poll(&f.g);
    CHECK(deliveries==before+3 && f.g.done_deferred==3 && f.g.retry_pending);
    for(unsigned i=0;i<3;++i)CHECK(c->local[i].done_due && c->local[i].busy);
    f.tt=f.th;qcom_glink_poll(&f.g);
    /* Take individually without consuming the other ids. */
    unsigned done=0;
    while(f.tt!=f.th){uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f.tx[(f.tt+i)%sizeof f.tx];
        CHECK(h[0]==QCOM_GLINK_RX_DONE_W_REUSE && get32(h+4)==done+1);++done;f.tt=(f.tt+8)%sizeof f.tx;}
    CHECK(done==3 && f.g.done_retried==3 && deliveries==before+3);
    qcom_glink_poll(&f.g);CHECK(!f.g.retry_pending && deliveries==before+3);
}
static void close_reopen(void)
{
    Fixture f;QcomGlinkChannel *c=setup(&f,1,1);
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    data(&f,QCOM_GLINK_TX_DATA,1,"lux!",4,0);qcom_glink_poll(&f.g);
    CHECK(c->local[0].done_due && deliveries==before+1);
    remote(&f,QCOM_GLINK_CLOSE,7,0,NULL,0);
    qcom_glink_poll(&f.g);
    CHECK(c->local[0].done_due && c->remote_open && f.rt!=f.rh);
    f.tt=f.th;qcom_glink_poll(&f.g);
    CHECK(take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==1);
    CHECK(!c->remote_open && !c->up && !c->local[0].done_due && !c->local[0].busy);
    const char name[]="IPCRTR";
    remote(&f,QCOM_GLINK_OPEN,8,sizeof name,(const uint8_t *)name,sizeof name);qcom_glink_poll(&f.g);
    CHECK(c->remote_open && c->rcid==8 && !c->up);
    take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1);
    remote(&f,QCOM_GLINK_OPEN_ACK,c->lcid,0,NULL,0);qcom_glink_poll(&f.g);
    CHECK(c->up && c->local[0].advertised);
    CHECK(take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==0);
    uint8_t payload[12];put32(payload,4);put32(payload+4,0);memcpy(payload+8,"lux!",4);
    remote(&f,QCOM_GLINK_TX_DATA,8,1,payload,sizeof payload);qcom_glink_poll(&f.g);
    CHECK(deliveries==before+2 && take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==1);
    qcom_glink_poll(&f.g);CHECK(deliveries==before+2 && take(&f,QCOM_GLINK_RX_DONE_W_REUSE,1)==0);
}
static void shared_channel_returns(void)
{
    Fixture f;QcomGlinkChannel *channels[3];channels[0]=setup(&f,1,1);
    const QcomGlinkClient client={NULL,receive};
    const char *names[2]={"fastrpcglink-apps-dsp","adsp_apps"};
    for(unsigned i=1;i<3;++i){
        channels[i]=qcom_glink_channel(&f.g,names[i-1],16,1,f.storage+16*i,&client,NULL,1);
        CHECK(channels[i]!=NULL);
        channels[i]->open_sent=channels[i]->open_acked=channels[i]->remote_open=channels[i]->up=1;
        channels[i]->rcid=(uint16_t)(7+i);channels[i]->local[0].advertised=1;
    }
    const unsigned before=deliveries;
    f.th=sizeof f.tx-24;
    uint8_t payload[12];put32(payload,4);put32(payload+4,0);memcpy(payload+8,"lux!",4);
    for(unsigned i=0;i<3;++i)remote(&f,QCOM_GLINK_TX_DATA,channels[i]->rcid,1,payload,sizeof payload);
    qcom_glink_poll(&f.g);
    CHECK(deliveries==before+3 && f.g.done_deferred==3 && f.g.retry_pending);
    for(unsigned i=0;i<3;++i)CHECK(channels[i]->local[0].done_due && channels[i]->local[0].busy);
    f.tt=f.th;qcom_glink_poll(&f.g);
    unsigned mask=0,done=0;
    while(f.tt!=f.th){
        uint8_t h[8];for(unsigned i=0;i<8;++i)h[i]=f.tx[(f.tt+i)%sizeof f.tx];
        const uint16_t cid=(uint16_t)(h[2]|h[3]<<8);
        unsigned slot=3;for(unsigned i=0;i<3;++i)if(channels[i]->lcid==cid)slot=i;
        CHECK(h[0]==QCOM_GLINK_RX_DONE_W_REUSE && get32(h+4)==1 && slot<3);
        if(slot<3){CHECK(!(mask&(1u<<slot)));mask|=1u<<slot;}
        ++done;f.tt=(f.tt+8)%sizeof f.tx;
    }
    CHECK(done==3 && mask==7 && f.g.done_retried==3 && deliveries==before+3);
    for(unsigned i=0;i<3;++i)CHECK(!channels[i]->local[0].done_due && !channels[i]->local[0].busy);
    qcom_glink_poll(&f.g);
    CHECK(!f.g.retry_pending && f.tt==f.th && deliveries==before+3);
}
int main(void)
{
    for(int reuse=0;reuse<=1;++reuse)for(int frag=0;frag<=1;++frag)completion(reuse,frag,1);
    intent_grant();
    multiple_returns();
    close_reopen();
    shared_channel_returns();
    printf("glink_return_host: %u checks, %u failures, %u delivered, %u kicks\n",checks,failures,deliveries,kicks);
    return failures?1:0;
}
