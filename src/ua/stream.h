#pragma once
#include "common.h"

static struct bbpUa_stream
// 接管一个 TCP 流。
{
    enum
    {
        __bbpUa_stream_sniffing_uaBegin,           // 正在寻找 http 头的结尾或者 ua 的开始，这时 buff_scan 中不应该有包
        __bbpUa_stream_sniffing_uaEnd,             // 已经找到 ua，正在寻找它的结尾，buff_scan 中可能有包
        __bbpUa_stream_waiting                     // 已经找到 ua 的结尾或者 http 头的结尾并且还没有 psh，接下来的包都直接放行
    } status;
    enum
    {
        __bbpUa_stream_scan_noFound,               // 还没找到 ua 的开头
        __bbpUa_stream_scan_uaBegin,               // 匹配到了 ua 开头，但是 ua 实际的开头在下个数据包
        __bbpUa_stream_scan_uaRealBegin,           // 匹配到了 ua 开头，ua 实际的开头在这个数据包
        __bbpUa_stream_scan_uaEnd,                 // 匹配到了 ua 的结尾，并且需要修改 ua
        __bbpUa_stream_scan_uaGood,                // 匹配到了 ua 的结尾，但是不需要修改 ua
        __bbpUa_stream_scan_headEnd                // 匹配到了 http 头部的结尾，没有发现 ua
    } scan_status;                              // 记录扫描结果，仅由 __bbpUa_stream_scan 和 __bbpUa_stream_reset 设置，由 bbpUa_stream_execute 和 __bbpUa_stream_scan 读取
    u_int32_t id[3];                            // 按顺序存储客户地址、服务地址、客户端口、服务端口，已经转换字节序
    struct bbpUa_packet *buff_scan, *buff_disordered;      // 分别存储准备扫描的、因乱序而提前收到的数据包，都按照序号排好了
    int32_t seq_offset;                         // 序列号的偏移。使得 buff_scan 中第一个字节的编号为零。在 bbpUa_stream 中，序列号使用相对值；但在传给下一层时，使用绝对值
    bool active;                                // 是否仍然活动，流每次处理的时候会置为 true，每隔一段时间会删除标志为 false（说明它在这段时间里没有活动）的流，将标志为 true 的流的标志也置为 false。
    unsigned scan_headEnd_matched, scan_uaBegin_matched, scan_uaEnd_matched, *scan_uaPreserve_matched;
            // 记录现在已经匹配了多少个字节，仅由 __bbpUa_stream_scan 和 __bbpUa_stream_reset 使用
    uint32_t scan_uaBegin_seq, scan_uaEnd_seq;
            // 记录 ua 开头和结束的序列号，仅由 __bbpUa_stream_scan、__bbpUa_stream_reset 设置
    struct bbpUa_map* map;                         // 记录 ua 的位置，方便修改重传数据包，仅由 __bbpUa_stream_modify 使用
    struct bbpWorker* worker;
    struct bbpUa_stream *prev, *next;
};

static struct bbpUa_stream* bbpUa_stream_create(const struct bbpUa_packet*, struct bbpWorker* worker);
static void bbpUa_stream_delete(struct bbpUa_stream*);

static bool bbpUa_stream_belongTo(const struct bbpUa_stream*, const struct bbpUa_packet*);      // 判断一个数据包是否属于一个流
static unsigned bbpUa_stream_execute(struct bbpUa_stream*, struct bbpUa_packet*);               // 已知一个数据包属于这个流后，处理这个数据包

static int32_t __bbpUa_stream_seq_desired(const struct bbpUa_stream*);                 // 返回 buff_scan 中最后一个数据包的后继的第一个字节的相对序列号

static void __bbpUa_stream_scan(struct bbpUa_stream*, struct bbpUa_packet*);    // 对一个最新的包进行扫描
static void __bbpUa_stream_reset(struct bbpUa_stream*);                      // 重置扫描进度，包括将 buff_scan 中的包全部发出

struct bbpUa_stream* bbpUa_stream_create(const struct bbpUa_packet* bbpp, struct bbpWorker* worker)
{
    struct bbpUa_stream* bbps;
    if(bbpUa_setting_debug)
        printk("bbpUa_stream_new\n");
    bbps = (struct bbpUa_stream*)bbpCommon_malloc(sizeof(struct bbpUa_stream) + sizeof(unsigned) * bbpUa_setting_preserve_n);
    if(bbps == 0)
        return 0;
    bbps -> scan_uaPreserve_matched = (unsigned*)((void*)bbps + sizeof(struct bbpUa_stream));
    bbps -> status = __bbpUa_stream_sniffing_uaBegin;
    memcpy(bbps -> id, bbpp -> lid, 3 * sizeof(u_int32_t));
    bbps -> buff_scan = bbps -> buff_disordered = 0;
    bbps -> seq_offset = bbpUa_packet_seq(bbpp, 0);
    if(bbpUa_packet_syn(bbpp))
        bbps -> seq_offset++;
    bbps -> active = true;
    bbps -> map = 0;
    bbps -> worker = worker;
    bbps -> prev = bbps -> next = 0;
    __bbpUa_stream_reset(bbps);
    return bbps;
}
void bbpUa_stream_delete(struct bbpUa_stream* bbps)
{
    struct bbpUa_map* bbpm;
    if(bbpUa_setting_debug)
        printk("bbpUa_stream_delete\n");
    bbpUa_packet_dropl(&bbps -> buff_scan);
    bbpUa_packet_dropl(&bbps -> buff_disordered);
    for(bbpm = bbps -> map; bbpm != 0; bbpm = bbpm -> next)
        bbpUa_map_delete(bbpm);
    bbpFree(bbps);
}

bool bbpUa_stream_belongTo(const struct bbpUa_stream* bbps, const struct bbpUa_packet* bbpp)
{
    if(bbpUa_setting_debug)
        printk("bbpUa_stream_belongTo\n");
    return memcmp(bbps -> id, bbpp -> lid, 3 * sizeof(u_int32_t)) == 0;
}
unsigned bbpUa_stream_execute(struct bbpUa_stream* bbps, struct bbpUa_packet* bbpp)
// 不要害怕麻烦，咱们把每一种情况都慢慢写一遍。
{
    if(bbpUa_setting_debug)
        printk("bbp-ua: bbpUa_stream_execute start, judging %u ...\n", bbpUa_packet_seq(bbpp, 0));

    // 肯定需要更新活动情况
    bbps -> active = true;
    
    // 首先处理如果是 ack 的情况
    if(bbpp -> ack)
    {
        if(bbpUa_setting_debug)
            printk("ack packet\n");
        bbpUa_map_refresh(&bbps -> map, bbpUa_packet_seqAck(bbpp, 0));
        return NF_ACCEPT;
    }

    // 其它情况，首先放掉所有没有应用层数据的包
    if(bbpUa_packet_appLen(bbpp) == 0)
    {
        if(bbpUa_setting_debug)
            printk("empty packet\n");
        return NF_ACCEPT;
    }

    // 接下来从小到大考虑数据包的序列号的几种情况
    // 已经发出的数据包，使用已有的映射修改
    if(bbpUa_packet_seq(bbpp, bbps -> seq_offset) < 0)
    {
        if(bbpUa_setting_debug)
            printk("\tThe packet is re-transforming or has been modified.\n");
        bbpUa_map_modify(&bbps -> map, &bbpp);
        return NF_ACCEPT;
    }
    // 已经放到 buff_scan 中的数据包，丢弃
    if(bbpUa_packet_seq(bbpp, bbps -> seq_offset) < __bbpUa_stream_seq_desired(bbps))
    {
        if(bbpUa_setting_debug)
            printk("\tThe packet with same seq has been captured, return NF_DROP.\n");
        return NF_DROP;
    }
    // 恰好是 buff_scan 的后继数据包，这种情况比较麻烦，写到最后
    // 乱序导致还没接收到前继的数据包，放到 buff_disordered
    if(bbpUa_packet_seq(bbpp, bbps -> seq_offset) > __bbpUa_stream_seq_desired(bbps))
    {
        if(bbpUa_setting_debug)
            printk("\tThe packet is disordered, return NF_STOLEN.\n");
        bbpUa_packet_insert_auto(&bbps -> buff_disordered, bbpp, bbps -> seq_offset);
        return NF_STOLEN;
    }

    // 接下来是恰好是 buff_scan 的后继数据包的情况，先分状态讨论，再一起考虑 buff_disordered 中的包
    // if(bbpUa_packet_seq(bbpp, bbps -> seq_offset) == __bbpUa_stream_seq_desired(bbps))
    if(true)
    {
        // 因为一会儿可能还需要统一考虑 buff_disordered 中的包，因此不直接 return，将需要的返回值写到这里，最后再 return
        unsigned rtn = NF_ACCEPT;

        if(bbpUa_setting_debug)
            printk("\tThe packet is desired one, further judging.\n");

        // 接下来分析几种情况
        //      * sniffing_uaBegin 状态下，先扫描这个数据包，再看情况处理。需要考虑 scan_status 和是否有 psh。
        //          * 没有 psh 的情况：
        //              * noFound：更新 seq_offset，返回 NF_ACCEPT。
        //              * uaBegin：状态切换为 sniffing_uaEnd，更新 seq_offset，返回 NF_ACCEPT。
        //              * uaRealBegin：保留数据包，状态切换为 sniffing_uaEnd，返回 NF_STOLEN。
        //              * uaEnd：生成映射，修改数据包，重置扫描进度，状态切换为 waiting，更新 seq_offset，返回 NF_ACCEPT。
        //              * uaGood 或 headEnd：重置扫描进度，状态切换为 waiting，更新 seq_offset，返回 NF_ACCEPT。
        //          * 有 psh 的情况：
        //              * noFound、uaBegin、uaRealBegin、uaGood 或 headEnd：重置扫描进度，更新 seq_offset，返回 NF_ACCEPT。
        //              * uaEnd：生成映射，修改数据包，重置扫描进度，更新 seq_offset，返回 NF_ACCEPT。
        //      * sniffing_uaEnd 状态下，同样是先扫描数据包，然后再分情况处理。需要考虑 scan_status、是否有 psh 以及是否达到最大长度
        //          * 没有 psh 的情况：
        //              * uaBegin 或 uaRealBegin：如果到了最大长度，就发出警告（ua 最大长度可能太小），重置扫描进度，发出数据包，状态切换为 waiting，更新 seq_offset，返回 NF_ACCEPT；
        //                      否则，保留数据包，返回 NF_STOLEN。
        //              * uaEnd：生成映射，修改数据包，重置扫描进度，发出数据包，状态切换为 waiting，更新 seq_offset，返回 NF_ACCEPT。
        //              * uaGood：重置扫描进度，发出数据包，状态切换为 waiting，更新 seq_offset，返回 NF_ACCEPT。
        //          * 有 psh 的情况：
        //              * uaBegin、uaRealBegin 或 uaGood：重置扫描进度，发出数据包，状态切换为 sniffing_uaBegin，更新 seq_offset，返回 NF_ACCEPT。
        //              * uaEnd：生成映射，修改数据包，重置扫描进度，发出数据包，状态切换为 sniffing_uaBegin，更新 seq_offset，返回 NF_ACCEPT。
        //      * waiting 状态下，如果有 psh，则将状态切换为 sniffing_uaBegin，否则不切换；然后更新 seq_offset，返回 NF_ACCEPT 即可。

        if(bbps -> status == __bbpUa_stream_sniffing_uaBegin)
        {
            if(bbpUa_setting_debug)
                printk("\t\tsniffing_uaBegin\n");
            __bbpUa_stream_scan(bbps, bbpp);
            if(bbpUa_setting_debug)
            {
                if(bbps -> scan_status == __bbpUa_stream_scan_noFound)
                    printk("\t\tnoFound\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaBegin)
                    printk("\t\tuaBegin\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaRealBegin)
                    printk("\t\tuaRealBegin\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaEnd)
                    printk("\t\tuaEnd\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaGood)
                    printk("\t\tuaGood\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_headEnd)
                    printk("\t\theadEnd\n");
                if(bbpUa_packet_psh(bbpp))
                    printk("\t\tpsh\n");
            }
            if(!bbpUa_packet_psh(bbpp))
                switch (bbps -> scan_status)
                {
                case __bbpUa_stream_scan_noFound:
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_uaBegin:
                    bbps -> status = __bbpUa_stream_sniffing_uaEnd;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_uaRealBegin:
                    bbpUa_packet_insert_end(&bbps -> buff_scan, bbpp);
                    bbps -> status = __bbpUa_stream_sniffing_uaEnd;
                    rtn = NF_STOLEN;
                    break;
                case __bbpUa_stream_scan_uaEnd:
                    bbpUa_map_insert_end(&bbps -> map, bbpUa_map_new(bbps -> scan_uaBegin_seq, bbps -> scan_uaEnd_seq));
                    bbpUa_map_modify(&bbps -> map, &bbpp);
                    __bbpUa_stream_reset(bbps);
                    bbps -> status = __bbpUa_stream_waiting;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_uaGood:
                case __bbpUa_stream_scan_headEnd:
                    __bbpUa_stream_reset(bbps);
                    bbps -> status = __bbpUa_stream_waiting;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                }
            else
                switch (bbps -> scan_status)
                {
                case __bbpUa_stream_scan_noFound:
                case __bbpUa_stream_scan_uaBegin:
                case __bbpUa_stream_scan_uaRealBegin:
                case __bbpUa_stream_scan_uaGood:
                case __bbpUa_stream_scan_headEnd:
                    __bbpUa_stream_reset(bbps);
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_uaEnd:
                    bbpUa_map_insert_end(&bbps -> map, bbpUa_map_new(bbps -> scan_uaBegin_seq, bbps -> scan_uaEnd_seq));
                    bbpUa_map_modify(&bbps -> map, &bbpp);
                    __bbpUa_stream_reset(bbps);
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                }
        }
        else if(bbps -> status == __bbpUa_stream_sniffing_uaEnd)
        {
            if(bbpUa_setting_debug)
                printk("\t\tsniffing_uaEnd\n");
            __bbpUa_stream_scan(bbps, bbpp);
            if(bbpUa_setting_debug)
            {
                if(bbps -> scan_status == __bbpUa_stream_scan_noFound)
                    printk("\t\tnoFound\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaBegin)
                    printk("\t\tuaBegin\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaRealBegin)
                    printk("\t\tuaRealBegin\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaEnd)
                    printk("\t\tuaEnd\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_uaGood)
                    printk("\t\tuaGood\n");
                else if(bbps -> scan_status == __bbpUa_stream_scan_headEnd)
                    printk("\t\theadEnd\n");
                if(bbpUa_packet_psh(bbpp))
                    printk("\t\tpsh\n");
            }
            if(!bbpUa_packet_psh(bbpp))
                switch (bbps -> scan_status)
                {
                case __bbpUa_stream_scan_uaBegin:
                case __bbpUa_stream_scan_uaRealBegin:
                    if(bbpUa_packet_num(&bbps -> buff_scan) + 1 == len_ua)
                    {
                        printk("warning: len_ua may be too short.\n");
                        __bbpUa_stream_reset(bbps);
                        bbpUa_packet_sendl(&bbps -> buff_scan);
                        bbps -> status = __bbpUa_stream_waiting;
                        bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                        rtn = NF_ACCEPT;
                    }
                    else
                    {
                        bbpUa_packet_insert_end(&bbps -> buff_scan, bbpp);
                        rtn = NF_STOLEN;
                    }
                    break;
                case __bbpUa_stream_scan_uaEnd:
                    bbpUa_map_insert_end(&bbps -> map, bbpUa_map_new(bbps -> scan_uaBegin_seq, bbps -> scan_uaEnd_seq));
                    bbpUa_map_modify(&bbps -> map, &bbps -> buff_scan);
                    bbpUa_map_modify(&bbps -> map, &bbpp);
                    __bbpUa_stream_reset(bbps);
                    bbpUa_packet_sendl(&bbps -> buff_scan);
                    bbps -> status = __bbpUa_stream_waiting;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_uaGood:
                    __bbpUa_stream_reset(bbps);
                    bbpUa_packet_sendl(&bbps -> buff_scan);
                    bbps -> status = __bbpUa_stream_waiting;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_noFound:
                case __bbpUa_stream_scan_headEnd:
                    break;
                }
            else
                switch (bbps -> scan_status)
                {
                case __bbpUa_stream_scan_uaBegin:
                case __bbpUa_stream_scan_uaRealBegin:
                case __bbpUa_stream_scan_uaGood:
                    __bbpUa_stream_reset(bbps);
                    bbpUa_packet_sendl(&bbps -> buff_scan);
                    bbps -> status = __bbpUa_stream_sniffing_uaBegin;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_uaEnd:
                    bbpUa_map_insert_end(&bbps -> map, bbpUa_map_new(bbps -> scan_uaBegin_seq, bbps -> scan_uaEnd_seq));
                    bbpUa_map_modify(&bbps -> map, &bbps -> buff_scan);
                    bbpUa_map_modify(&bbps -> map, &bbpp);
                    __bbpUa_stream_reset(bbps);
                    bbpUa_packet_sendl(&bbps -> buff_scan);
                    bbps -> status = __bbpUa_stream_sniffing_uaBegin;
                    bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
                    rtn = NF_ACCEPT;
                    break;
                case __bbpUa_stream_scan_noFound:
                case __bbpUa_stream_scan_headEnd:
                    break;
                }
        }
        // else if(bbps -> status == __bbpUa_stream_waiting)
        else
        {
            if(bbpUa_setting_debug)
            {
                printk("\t\tsniffing_uaBegin\n");
                if(bbpUa_packet_psh(bbpp))
                    printk("\t\tpsh.\n");
            }
            if(bbpUa_packet_psh(bbpp))
                bbps -> status = __bbpUa_stream_sniffing_uaBegin;
            bbpUa_packet_makeOffset(bbpp, &bbps -> seq_offset);
            rtn = NF_ACCEPT;
        }

        // 接下来考虑乱序的包
        while(bbps -> buff_disordered != 0)
        {
            // 序列号是已经发出去的，丢弃
            if(bbpUa_packet_seq(bbps -> buff_disordered, bbps -> seq_offset) < __bbpUa_stream_seq_desired(bbps))
            {
                if(bbpUa_setting_debug)
                    printk("\tdrop an disordered packet.\n");
                bbpUa_packet_drop(bbpUa_packet_pop_begin(&bbps -> buff_disordered));
            }
            // 如果序列号过大，结束循环
            else if(bbpUa_packet_seq(bbps -> buff_disordered, bbps -> seq_offset) > __bbpUa_stream_seq_desired(bbps))
                break;
            // 如果序列号恰好，把它从链表中取出，然后像刚刚抓到的包那样去执行
            else
            {
                // 将包从链表中取出
                struct bbpUa_packet* bbpp2;
                unsigned rtn;
                if(bbpUa_setting_debug)
                    printk("\texecute a disordered packet.\n");
                bbpp2 = bbpUa_packet_pop_begin(&bbps -> buff_disordered);
                rtn = bbpUa_stream_execute(bbps, bbpp2);
                if(bbpUa_setting_debug)
                {
                    if(rtn == NF_ACCEPT)
                        printk("\t\treturn NF_ACCEPT.\n");
                    else if(rtn == NF_DROP)
                        printk("\t\treturn NF_DROP.\n");
                    else if(rtn == NF_STOLEN)
                        printk("\t\treturn NF_STOLEN.\n");
                }
                if(rtn == NF_ACCEPT)
                    bbpUa_packet_send(bbpp2);
                else if(rtn == NF_DROP)
                    bbpUa_packet_drop(bbpp2);
                else if(rtn == NF_STOLEN);
            }
        }
        
        if(bbpUa_setting_debug)
        {
            if(rtn == NF_ACCEPT)
                printk("\treturn NF_ACCEPT.\n");
            else if(rtn == NF_DROP)
                printk("\treturn NF_DROP.\n");
            else if(rtn == NF_STOLEN)
                printk("\treturn NF_STOLEN.\n");
        }
        return rtn;
    }
}

int32_t __bbpUa_stream_seq_desired(const struct bbpUa_stream* bbps)
{
    struct bbpUa_packet* bbpp = bbps -> buff_scan;
    if(bbpUa_setting_debug)
        printk("bbpUa_stream_seq_desired\n");
    if(bbpp == 0)
        return 0;
    else
    {
        for(; bbpp -> next != 0; bbpp = bbpp -> next);
        return bbpUa_packet_seq(bbpp, bbps -> seq_offset) + bbpUa_packet_appLen(bbpp);
    }
}

void __bbpUa_stream_scan(struct bbpUa_stream* bbps, struct bbpUa_packet* bbpp)
{
    unsigned char* p = bbpUa_packet_appBegin(bbpp);
    if(bbpUa_setting_debug)
        printk("bbpUa_stream_scan\n");

    // 需要匹配的字符串包括：headEnd、uaBegin、uaEnd、uaPreserve
    // 开始这个函数时，scan_status 只可能是 noFound、uaBegin 或 uaRealBegin（这两个可以无差别对待）
    //      * noFound：扫描 uaBegin、headEnd，当匹配到其中一个时停下来开始决策
    //          * uaBegin：如果已经到数据包末尾，则将状态设置为 uaBegin，写入 scan_uaBegin_seq，返回；否则，将状态设置为 uaRealBegin，写入 scan_uaBegin_seq，继续下个阶段的扫描
    //          * headEnd：将状态设置为 headEnd，返回
    //      * uaBegin 或 uaRealBegin：扫描 uaEnd、uaPreserve，匹配到其中一个时停下来开始决策
    //          * uaEnd：将状态设置为 uaEnd，设置 scan_uaEnd_seq，返回
    //          * uaPreserve：将状态设置为 uaGood，返回

    if(bbps -> scan_status == __bbpUa_stream_scan_noFound)
        for(; p != bbpUa_packet_appEnd(bbpp); p++)
        {
            if(*p == str_uaBegin[bbps -> scan_uaBegin_matched])
            {
                bbps -> scan_uaBegin_matched++;
                if(bbps -> scan_uaBegin_matched == strlen(str_uaBegin))
                {
                    if(p + 1 == bbpUa_packet_appEnd(bbpp))
                        bbps -> scan_status = __bbpUa_stream_scan_uaBegin;
                    else
                        bbps -> scan_status = __bbpUa_stream_scan_uaRealBegin;
                    bbps -> scan_uaBegin_seq = bbpUa_packet_seq(bbpp, 0) + ((p + 1) - bbpUa_packet_appBegin(bbpp));
                    if(bbpUa_setting_debug)
                        printk("uaBegin_seq %u\n", bbps -> scan_uaBegin_seq);
                    p++;
                    break;
                }
            }
            else
                bbps -> scan_uaBegin_matched = 0;
            if(*p == str_headEnd[bbps -> scan_headEnd_matched])
            {
                bbps -> scan_headEnd_matched++;
                if(bbps -> scan_headEnd_matched == strlen(str_headEnd))
                {
                    bbps -> scan_status = __bbpUa_stream_scan_headEnd;
                    return;
                }
            }
            else
                bbps -> scan_headEnd_matched = 0;
        }

    if(bbps -> scan_status == __bbpUa_stream_scan_uaBegin || bbps -> scan_status == __bbpUa_stream_scan_uaRealBegin)
        for(; p != bbpUa_packet_appEnd(bbpp); p++)
        {
            unsigned i;
            if(*p == str_uaEnd[bbps -> scan_uaEnd_matched])
            {
                bbps -> scan_uaEnd_matched++;
                if(bbps -> scan_uaEnd_matched == strlen(str_uaEnd))
                {
                    bbps -> scan_status = __bbpUa_stream_scan_uaEnd;
                    bbps -> scan_uaEnd_seq = bbpUa_packet_seq(bbpp, 0) + ((p + 1) - bbpUa_packet_appBegin(bbpp)) - strlen(str_uaEnd);
                    if(bbpUa_setting_debug)
                        printk("uaEnd_seq %u\n", bbps -> scan_uaEnd_seq);
                    return;
                }
            }
            else
                bbps -> scan_uaEnd_matched = 0;
            for(i = 0; i < n_str_preserve; i++)
            {
                if(*p == str_preserve[i][bbps -> scan_uaPreserve_matched[i]])
                {
                    bbps -> scan_uaPreserve_matched[i]++;
                    if(bbps -> scan_uaPreserve_matched[i] == strlen(str_preserve[i]))
                    {
                        bbps -> scan_status = __bbpUa_stream_scan_uaGood;
                        return;
                    }
                }
                else
                    bbps -> scan_uaPreserve_matched[i] = 0;
            }
        }
}
void __bbpUa_stream_reset(struct bbpUa_stream* bbps)
{
    if(bbpUa_setting_debug)
        printk("bbpUa_stream_reset\n");
    bbps -> scan_status = __bbpUa_stream_scan_noFound;
    bbps -> scan_headEnd_matched = bbps -> scan_uaBegin_matched = bbps -> scan_uaEnd_matched = 0;
    memset(bbps -> scan_uaPreserve_matched, 0, sizeof(unsigned) * n_str_preserve);
}
