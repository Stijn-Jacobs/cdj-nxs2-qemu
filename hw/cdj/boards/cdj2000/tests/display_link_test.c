/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual board MMIO/send/timer paths with deterministic memory, time and IRQs. */
#define CDJ_DISPLAY_LINK_TEST 1
#include "../display_link.c"

static int dma_irq,serial_irq;
static const char *test_out="/tmp";
static unsigned deliveries;
static uint8_t delivered[4][LINK_BUFFER];
static size_t delivered_len[4];
static bool reenter,prior_status;
static void qemu_set_irq(qemu_irq irq,int level)
{
    if (irq) {
        if (level && (irq==&dma_irq || irq==&serial_irq)) {assert(deliveries || prior_status);}
        *irq=!!level;
    }
}
static void cdj2000_display_send(const uint8_t *packet,size_t len)
{
    assert(deliveries<4 && len<=LINK_BUFFER);
    /* No new completion or serial event may precede actual delivery. */
    assert(!dma_irq && !serial_irq && link_tx.units==0);
    assert(!(link_tx.reg[SER_EVENT/4]&SER_TX_DONE));
    if (link_fault.kind && !link_fault.applied && link_fault.report) {assert(ftell(link_fault.report)==0);}
    if (reenter) {
        uint8_t before[LINK_BUFFER];memcpy(before,packet,len);
        link_write(&link_tx,DMA_CTRL,0,4);
        link_write(&link_tx,DMA_CTRL,CTRL_START,4);
        assert(!memcmp(before,packet,len));
    }
    memcpy(delivered[deliveries],packet,len);delivered_len[deliveries++]=len;
}
static void fresh(uint32_t delay_us)
{
    if (link_fault.report) {fclose(link_fault.report);}
    memset(&link_fault,0,sizeof(link_fault));
    memset(&link_tx,0,sizeof(link_tx));memset(&link_rx,0,sizeof(link_rx));
    memset(test_timers,0,sizeof(test_timers));test_timer_count=0;
    test_now=1000;test_errors=0;test_last_log[0]=0;
    deliveries=0;dma_irq=0;serial_irq=0;reenter=false;prior_status=false;
    link_log=true;link_log_full=false;link_tx_delay_us=delay_us;
    link_port_init(&link_tx,NULL,"test",0,0x1008,&dma_irq,&serial_irq);
    link_tx.reg[link_tx.addr_reg/4]=0;
    link_tx.reg[link_tx.len_reg/4]=64;
    link_tx.reg[SER_CTRL/4]=SER_TX_DONE;
    for (unsigned i=0;i<sizeof(test_memory);i++) {test_memory[i]=(uint8_t)i;}
}
static void start(void) {link_write(&link_tx,DMA_CTRL,CTRL_START,4);}
static void ack(void)
{
    while (link_tx.units) {link_write(&link_tx,DMA_STATUS,STATUS_UNIT,4);}
    link_write(&link_tx,SER_EVENT,0,4);assert(!dma_irq && !serial_irq);
}
static void at(int64_t now)
{
    test_now=now;
    for (unsigned i=0;i<test_timer_count;i++) {
        QEMUTimer *t=&test_timers[i];
        if (t->armed && t->due<=now) {t->armed=false;t->callback(t->opaque);}
    }
}
/* Encoder fields below follow native.rs Header and browser.rs
 * PageRequest::encode/Page::encode_compact. Payload is a valid one-row ROOT
 * page (ROOT request, Navigation row with empty terminated label). */
static void put16(uint8_t *p,unsigned n) {p[0]=n;p[1]=n>>8;}
static void put32(uint8_t *p,uint32_t n) {put16(p,n);put16(p+2,n>>16);}
static void crc(uint8_t *p,unsigned len)
{
    uint16_t v=0;
    for (unsigned n=0;n<len-2;n++) {v^=(uint16_t)p[n]<<8;for (unsigned b=0;b<8;b++) {v=(v<<1)^((v&0x8000)?0x1021:0);}}
    put16(p+len-2,v);assert(fault_crc(p,len));
}
static unsigned native_packet(bool wave,unsigned unit)
{
    unsigned payload=wave?892:86,len=(40+payload+2+15)&~15;
    memset(test_memory,0,len);memcpy(test_memory,"DLNK",4);
    test_memory[4]=3;test_memory[5]=1;test_memory[6]=wave?3:2;test_memory[7]=3;
    put32(test_memory+8,1);put32(test_memory+12,2);put32(test_memory+16,7);
    put16(test_memory+26,payload);put32(test_memory+28,9);
    put16(test_memory+32,wave?0x103:0x110);put16(test_memory+34,len);
    if (wave) {put32(test_memory+40,unit);} else {
        put32(test_memory+40,66304);put32(test_memory+44,5);put32(test_memory+48,1);
        put32(test_memory+56,99);put32(test_memory+60,6);test_memory[68]=6;
        put32(test_memory+84,1);test_memory[88]=1;test_memory[89]=1;
        put32(test_memory+92,7);test_memory[96]=2;put32(test_memory+108,1);
    }
    crc(test_memory,len);link_tx.reg[link_tx.len_reg/4]=len;return len;
}
static void enable_fault(LinkFaultCase kind)
{
    static const char *names[]={NULL,"native-root-crc","native-wave-truncate","stock-browser-crc","stock-wave-truncate"};
    link_fault.kind=kind;link_fault.name=names[kind];link_fault.report=tmpfile();assert(link_fault.report);
}
static char report[20000];
static FILE *report_reader(void) {return fdopen(dup(fileno(link_fault.report)),"r");}
static void read_report(FILE *f)
{
    rewind(f);size_t n=fread(report,1,sizeof(report)-1,f);report[n]=0;assert(!ferror(f));fclose(f);
    assert(strstr(report,"\"scope\":\"submitted_to_sport\""));
    assert(strstr(report,"\"applied_count\":1"));assert(strstr(report,"\"matches\":1"));
    assert(strstr(report,"\"overflow\":0"));
    if (strstr(report,"\"refused\":0")) {
        char path[256];snprintf(path,sizeof(path),"%s/link-fault.%s.sample.json",test_out,link_fault.name);
        FILE *sample=fopen(path,"w");assert(sample);fputs(report,sample);assert(!fclose(sample));
    }
}
static void announce(unsigned command,unsigned bytes)
{
    memset(test_memory,0,64);put16(test_memory+58,command);put16(test_memory+60,bytes/2);crc(test_memory,64);
    link_tx.reg[link_tx.len_reg/4]=64;start();ack();assert(!link_fault.applied);
}
static void stock_packet(unsigned command,unsigned unit,unsigned len)
{
    memset(test_memory,0,len);put16(test_memory,command);put32(test_memory+2,unit);
    crc(test_memory,len);link_tx.reg[link_tx.len_reg/4]=len;
}
static void fault_tests(void)
{
    const char *vars[]={"CDJ_LINK_TX_DELAY_US","CDJ_LINK_RX_GAP_US","CDJ_BF531_PPI_DELAY_US","CDJ_BF531_PPI_DELAY_COUNT","CDJ_LINK_CAPTURE"};
    for (unsigned n=0;n<5;n++) {unsetenv(vars[n]);}
    char path[256];snprintf(path,sizeof(path),"%s/link-fault-test-%ld.json",test_out,(long)getpid());unlink(path);
    fresh(0);assert(link_fault_configure(NULL,NULL));
    assert(!link_fault_configure("",path));assert(!link_fault_configure("native-root-crc",NULL));
    assert(!link_fault_configure(NULL,path));assert(!link_fault_configure("other",path));
    assert(!link_fault_configure("native-root-crc","relative.json"));
    assert(!link_fault_configure("native-root-crc","/missing/parent/report.json"));
    setenv("CDJ_LINK_RX_GAP_US","0",1);assert(!link_fault_configure("native-root-crc",path));unsetenv("CDJ_LINK_RX_GAP_US");
    for (unsigned n=0;n<3;n++) {
        const char *bad[]={"", "x", "1"};setenv("CDJ_BF531_PPI_DELAY_US",bad[n],1);
        assert(!link_fault_configure("native-root-crc",path));
    }
    setenv("CDJ_BF531_PPI_DELAY_US","0",1);
    link_tx_delay_us=1;assert(!link_fault_configure("native-root-crc",path));link_tx_delay_us=0;
    assert(link_fault_configure("native-root-crc",path));
    assert(!link_fault_configure("native-root-crc",path)); /* no overwrite */
    assert(vmstate_link_port.version_id==3 && vmstate_link_port.pre_save(&link_tx)==-EINVAL);
    assert(vmstate_link_port.pre_load(&link_tx)==-EINVAL);
    assert(vmstate_link_port.post_load(&link_tx,3)==-EINVAL);
    fclose(link_fault.report);link_fault.report=NULL;unlink(path);unsetenv("CDJ_BF531_PPI_DELAY_US");

    fresh(0);enable_fault(FAULT_NATIVE_ROOT_CRC);unsigned len=native_packet(false,0);
    uint8_t original[LINK_BUFFER];memcpy(original,test_memory,len);
    /* Reject original corruption and every individual selector mismatch. */
    for (unsigned n=0;n<16;n++) {
        unsigned positions[]={4,5,6,7,8,12,16,28,32,36,48,52,88,89,90,91};
        memcpy(test_memory,original,len);
        if (positions[n]==12 || positions[n]==16 || positions[n]==28) {put32(test_memory+positions[n],0);}
        else if (positions[n]==89) {test_memory[89]=2;}
        else {test_memory[positions[n]]^=1;}
        crc(test_memory,len);
        assert(!fault_select(test_memory,len));
    }
    memcpy(test_memory,original,len);test_memory[40]^=1;assert(!fault_select(test_memory,len));
    memcpy(test_memory,original,len);FILE *f=report_reader();link_cap=tmpfile();assert(link_cap);
    start();assert(deliveries==1 && link_fault.applied && delivered_len[0]==len);
    assert(!memcmp(test_memory,original,len));assert(delivered[0][40]==(original[40]^1));
    for (unsigned n=0;n<len;n++) {if (n!=40) {assert(delivered[0][n]==original[n]);}}
    rewind(link_cap);uint8_t captured[LINK_BUFFER+8];assert(fread(captured,1,len+8,link_cap)==len+8);
    assert(!memcmp(captured,"SPRX",4) && fault_u32(captured+4)==len && !memcmp(captured+8,delivered[0],len));
    fclose(link_cap);link_cap=NULL;
    read_report(f);assert(strstr(report,"\"context\":1,\"context_key\":0,\"row_count\":1"));
    char sample_path[512];snprintf(sample_path,sizeof(sample_path),"%s/link-fault.sample.json",test_out);
    FILE *sample=fopen(sample_path,"w");assert(sample);fputs(report,sample);assert(!fclose(sample));
    ack();start();assert(deliveries==2 && !memcmp(delivered[1],original,len));ack();

    fresh(0);enable_fault(FAULT_NATIVE_WAVE_TRUNCATE);len=native_packet(true,1);
    assert(!fault_select(test_memory,len));native_packet(true,2);memcpy(original,test_memory,len);
    put16(test_memory+26,891);crc(test_memory,len);assert(!fault_select(test_memory,len));
    memcpy(test_memory,original,len);put32(test_memory+36,1);crc(test_memory,len);assert(!fault_select(test_memory,len));
    memcpy(test_memory,original,len);test_memory[len-4]=1;crc(test_memory,len);assert(!fault_select(test_memory,len));
    memcpy(test_memory,original,len);test_memory[len-1]^=1;assert(!fault_select(test_memory,len));
    memcpy(test_memory,original,len);

    f=report_reader();start();assert(delivered_len[0]==len-16 && link_tx.units==len/16);
    assert(!memcmp(delivered[0],original,len-16) && !memcmp(test_memory,original,len));
    read_report(f);assert(strstr(report,"\"unit\":2"));ack();start();assert(delivered_len[1]==len && fault_crc(delivered[1],len));ack();

    fresh(0);enable_fault(FAULT_STOCK_BROWSER_CRC);
    stock_packet(0x10,0,64);assert(!fault_select(test_memory,64)); /* no announcement */
    memset(test_memory,0,64);put16(test_memory+58,0x10);put16(test_memory+60,40);crc(test_memory,64);
    assert(!fault_select(test_memory,64));stock_packet(0x10,0,64);
    assert(!fault_select(test_memory,64)); /* wrong announced extent */
    announce(0x10,64);stock_packet(0x10,0,64);
    test_memory[20]^=1;assert(!fault_select(test_memory,64));test_memory[20]^=1;

    memcpy(original,test_memory,64);f=report_reader();start();assert(deliveries==2 && link_fault.applied);
    assert(delivered[1][20]==1 && !fault_crc(delivered[1],64) && !memcmp(test_memory,original,64));
    read_report(f);assert(strstr(report,"\"command\":16,\"announced_length\":64,\"unit\":null,\"prior_unit1\":false"));ack();

    fresh(0);enable_fault(FAULT_STOCK_WAVE_TRUNCATE);announce(0x20,944);stock_packet(0x20,2,944);
    assert(!fault_select(test_memory,944));stock_packet(0x20,1,944);start();ack();
    assert(link_fault.prior_unit1 && !link_fault.applied);stock_packet(0x20,2,944);
    f=report_reader();start();assert(delivered_len[2]==928 && link_tx.units==59);
    read_report(f);assert(strstr(report,"\"prior_unit1\":true"));ack();

    /* Direct low-level delay path remains ordered, although public combined
     * stress configuration is refused. Guard original bytes during reentry. */
    fresh(1);enable_fault(FAULT_NATIVE_ROOT_CRC);len=native_packet(false,0);f=report_reader();
    start();assert(!link_fault.applied && !deliveries);reenter=true;at(link_tx.tx_due_ns);
    assert(link_fault.applied && deliveries==1 && link_tx.tx_refused==2);
    read_report(f);assert(strstr(report,"\"refused\":2"));ack();
    fresh(0);enable_fault(FAULT_NATIVE_ROOT_CRC);native_packet(false,0);reenter=true;f=report_reader();
    start();assert(deliveries==1 && link_tx.tx_refused==2);read_report(f);assert(strstr(report,"\"refused\":2"));ack();
    fresh(0);
    pid_t child=fork();assert(child>=0);
    if (!child) {
        enable_fault(FAULT_NATIVE_ROOT_CRC);fclose(link_fault.report);link_fault.report=fopen("/dev/null","r");assert(link_fault.report);
        native_packet(false,0);start();_exit(0);
    }
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==1);
}

int main(int argc,char **argv)
{
    assert(argc==1 || argc==2);if (argc==2) {test_out=argv[1];}
    uint32_t parsed=99;
    assert(link_us_parse(NULL,&parsed) && parsed==0);
    assert(link_us_parse("0",&parsed) && parsed==0);
    assert(link_us_parse("50000",&parsed) && parsed==50000);
    for (unsigned i=0;i<8;i++) {
        const char *bad[]={"","-1","+1"," 1","1 ","50001","9999999999999999999999","1us"};
        parsed=17;assert(!link_us_parse(bad[i],&parsed) && parsed==17);
    }
    fresh(0);start();assert(deliveries==1 && delivered_len[0]==64);
    for (unsigned i=0;i<64;i++) {assert(delivered[0][i]==(uint8_t)i);}
    assert(link_tx.units==4 && dma_irq && serial_irq);
    assert(!(link_tx.reg[DMA_CTRL/4]&CTRL_START));assert(!link_tx.tx_pending);
    assert(!link_tx.tx_applied && !link_tx.tx_delay->armed);
    assert(!vmstate_link_port.pre_save(&link_tx) && !vmstate_link_port.post_load(&link_tx,3));ack();

    fresh(5000);start();int64_t due=5001000;
    assert(link_tx.tx_pending && link_tx.tx_scheduled_ns==1000 && link_tx.tx_due_ns==due);
    assert(!deliveries && !link_tx.units && !dma_irq && !serial_irq);
    assert(link_tx.reg[DMA_CTRL/4]&CTRL_START);
    memset(test_memory,0xff,64); /* MAIN may reuse memory only after completion;
                                * copied ownership still protects captured data. */
    at(due-1);assert(!deliveries && !link_tx.tx_applied);
    link_tx_delay_up(&link_tx);assert(!deliveries); /* premature/stale timer callback */
    at(due);assert(deliveries==1 && !link_tx.tx_pending && link_tx.tx_applied==1);
    for (unsigned i=0;i<64;i++) {assert(delivered[0][i]==(uint8_t)i);}
    assert(link_tx.units==4 && dma_irq && serial_irq);
    assert(strstr(test_last_log,"delay_us=5000 scheduled_ns=1000 due_ns=5001000 delivered_ns=5001000 applied=1"));
    link_tx_delay_up(&link_tx);assert(deliveries==1 && link_tx.tx_applied==1);ack();
    test_now=6000000;memset(test_memory,0x5a,64);start();
    at(11000009);assert(deliveries==2 && link_tx.tx_applied==2);
    assert(strstr(test_last_log,"due_ns=11000000 delivered_ns=11000009 applied=2"));
    assert(delivered[1][0]==0x5a && delivered[0][0]==0);ack();
    assert(vmstate_link_port.pre_save(&link_tx)==-EINVAL);
    assert(vmstate_link_port.pre_load(&link_tx)==-EINVAL);
    assert(vmstate_link_port.post_load(&link_tx,3)==-EINVAL);

    /* Starting a delayed packet does not erase an earlier transaction's
     * unacknowledged DMA/serial status or pretend it is this packet's EOF. */
    fresh(1);link_tx.units=3;link_tx.reg[SER_EVENT/4]=SER_TX_DONE;
    dma_irq=1;serial_irq=1;prior_status=true;start();
    assert(!deliveries && link_tx.units==3 && dma_irq && serial_irq);
    assert(link_tx.reg[SER_EVENT/4]==SER_TX_DONE);ack();prior_status=false;
    at(link_tx.tx_due_ns);assert(deliveries==1 && link_tx.tx_applied==1);ack();

    fresh(1);start();int64_t original=link_tx.tx_due_ns;
    link_write(&link_tx,DMA_CTRL,0,4);start();start();
    assert(test_errors==3 && link_tx.tx_refused==3 && link_tx.tx_pending);
    assert(link_tx.tx_due_ns==original && !deliveries);
    reenter=true;at(original);assert(deliveries==1 && link_tx.tx_applied==1);
    assert(link_tx.tx_refused==5 && test_errors==5);ack();

    fresh(50000);link_tx.reg[link_tx.len_reg/4]=LINK_BUFFER+16;start();
    at(50001000);assert(deliveries==1 && delivered_len[0]==LINK_BUFFER);
    assert(link_tx.units==LINK_BUFFER/LINK_UNIT);ack();
    fresh(1);test_now=INT64_MAX-999;start();
    assert(test_errors==1 && !link_tx.tx_pending && !deliveries);
    assert(!(link_tx.reg[DMA_CTRL/4]&CTRL_START));
    fault_tests();
    puts("display_link_test: four one-shot CRC/truncation fault cases and exact witnesses passed");
    puts("display_link_test: delay config, default delivery, copied bytes, exact timer/IRQ ordering, bounded reentry and snapshot refusal passed");
    return 0;
}
