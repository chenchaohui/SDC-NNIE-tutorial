#include <iostream>
#include <fcntl.h>
#include <unistd.h>
#include <sys/uio.h>
#include <cstring>
#include <thread>
#include "hi_type.h"
#include "common.hpp"

using namespace std;
#define QUEUE_EMPTY -2
#define QUEUE_FULL -3

class ArrayQueue {
    /*万用数组队列,write_idx应永远大于等于read_idx,
     *实际的数组容量会比数组大小少1,因为write_idx指向的位置恒定为空*/
private:
    int m_read_idx;//读取下标
    int m_write_idx;//写入下标
    long long m_data_idx;//数据总量下标
    int m_item_size;//单个数据的大小
    int m_capacity;//数组容量
    pthread_mutex_t m_mutex;//线程锁
    char *m_buff;//缓存

public:
    ArrayQueue(int capacity, int item_size) {
        pthread_mutex_init(&m_mutex, NULL);
        m_read_idx = 0;
        m_write_idx = 0;
        m_data_idx = 0;
        m_capacity = capacity+1;
        m_item_size = item_size;
        m_buff = new char[m_capacity * m_item_size];
    }

    ~ArrayQueue() {
        delete m_buff;
    }
    int put(void *item) {
        /*先判断+1后是否队列已满,不为满则添加数据*/
        pthread_mutex_lock(&m_mutex);
        /*整体数据下标这个参数用于判断我们是否由于处理不及时漏掉了一部分视频帧,因此无论如何都要+1*/
        m_data_idx++;
        int ret = PAS;
        int next = (m_capacity + m_write_idx + 1) % m_capacity;
        if (next == m_read_idx) {
            ret = QUEUE_FULL;
        } else {
            void *ptr = m_buff + m_write_idx * m_item_size;
            memcpy(ptr, item, m_item_size);
            m_write_idx = next;
        }
        pthread_mutex_unlock(&m_mutex);
        return ret;
    }

    int get(void *item, int num = 1, long long *data_idx=NULL) {
        /*取数据,可以连续取num个数据*/
        /*先判断队列是否为空,不为空则取数据后指针+1*/
        pthread_mutex_lock(&m_mutex);
        int ret = PAS;
        int idx, read_idx;
        void *ptr;
        if ((m_read_idx == m_write_idx) || (m_write_idx + m_capacity - m_read_idx) % m_capacity < num) {
            ret = QUEUE_EMPTY;
        } else {
            DEBUG_LOG("m_read_idx: %i", m_read_idx);
            read_idx = m_read_idx;
            for (idx = 0; idx < num; idx++) {
                ptr = m_buff + read_idx * m_item_size;
                memcpy((char*)item+idx*m_item_size, ptr, m_item_size);
                read_idx = (m_capacity + read_idx + 1) % m_capacity;
            }
            /*在连续取数据的情况下read指针也只+1*/
            m_read_idx = (m_capacity + m_read_idx + 1) % m_capacity;
            if (data_idx != NULL) {
                *data_idx = m_data_idx;
            }
        }
        pthread_mutex_unlock(&m_mutex);
        return ret;
    }

};

class VideoService {
private:
    int m_fd_video=-1; //sdc的视频服务句柄
    long long m_frame_idx=-1;//用于保存取出数据的帧序号
    uint32_t m_yuv_channel_id=0; //yuv视频通道id
    ArrayQueue *m_video_queue; //缓存视频数据的队列
    int m_stop_reading=0;


public:
    void stop_reading(){
        m_stop_reading=1;
    }

    explicit VideoService(ArrayQueue *video_queue) {
        m_video_queue = video_queue;
        service_register();
        get_yuv_channel_id();
    }

    int service_register() {
        m_fd_video = open("/mnt/srvfs/video.iaas.sdc", O_RDWR);
        if (m_fd_video < 0) {
            DEBUG_LOG("open video failed, ret is: %i\n", m_fd_video);
            return ERR;
        }
        return PAS;
    }

    int get_yuv_channel_id() {
        /*申请一个摄像头视频通道的句柄,sdc会通过这个句柄传输图像*/
        int ret, idx;
        /*申请一段空间用于保存通讯请求的内容,一条请求中包含数个部分:
         * 请求头(SDC_COMMON_HEAD_S),扩展请求头(SDC_EXTEND_HEAD_S),请求内容(CONTENT)*/
        char buf[4096] = {0};
        /*声明一个请求头结构体指针*/
        SDC_COMMON_HEAD_S *common_head;
        /*声明一个请求内容结构体指针,这里是用于保存yuv channel相关的信息*/
        SDC_CHAN_QUERY_S *channel_query;
        SDC_YUV_CHANNEL_INFO_S *yuv_channel_info;

        /*将请求头指针与通讯指针对齐*/
        common_head = (SDC_COMMON_HEAD_S *) buf;
        /*指定请求头内容,不同的请求头会向SDC(摄像头)说明本次请求的类型*/
        common_head->version = SDC_VERSION;
        common_head->url = SDC_URL_YUV_CHANNEL;//说明这是YUV_CHANNEL相关的请求
        common_head->method = SDC_METHOD_GET;//说明是获取内容相关的操作
        common_head->head_length = sizeof(SDC_COMMON_HEAD_S);//将请求的不同部分进行分割,这里就是说明请求头的部分占据了多少byte
        common_head->content_length = 0;

        /*写入通讯请求*/
        ret = write(m_fd_video, buf, common_head->head_length);
        if (ret < 0) {
            DEBUG_LOG("ERR: ERR:writev fail yuv_channel_get,response:%d,url:%d,code:%d,method:%d\n",
                      common_head->response,
                      common_head->url,
                      common_head->code,
                      common_head->method);
            return ERR;
        }

        /*将请求内容与通讯指针对齐,因为上文请求头已经占用了一部分空间了,这里要从请求头后的空间进行对齐
        * 这个样例中请求内容是用于保存返回结果的,因此不需要对结构体进行任何赋值*/
        yuv_channel_info = (SDC_YUV_CHANNEL_INFO_S *) (buf + common_head->head_length);
        /*获取句柄返回的请求结果*/
        ret = read(m_fd_video, buf, sizeof(buf));
        if (ret < 0 || common_head->code != SDC_CODE_200) {
            printf("ERR: get_yuv_channel_id read fail response:%d,url:%d,code:%d, method:%d\n",
                   common_head->response,
                   common_head->url,
                   common_head->code,
                   common_head->method);
            return ERR;
        }
        /*默认会返回所有可用的yuv channel,根据content length的大小判断返回了多少*/
        for (idx = 0; idx < (common_head->content_length / sizeof(*yuv_channel_info)); idx++) {
            if ((yuv_channel_info->is_snap_channel == 0)/*不为抓拍通道*/
                && (yuv_channel_info->subscribe_cnt == 0)/*订阅量为0*/
                && (yuv_channel_info->nResolutionModitfy == 1)/*可以修改分辨率*/
                    ) {
                m_yuv_channel_id = yuv_channel_info->param.channel;
                DEBUG_LOG("INFO:Get yuv video channel id：%d\n", yuv_channel_info->param.channel);
                return PAS;
            }
        }
        /*如果上面的for循环没有返回,就表示没有找到可用的通道*/
        DEBUG_LOG("ERR: No available channel found.\n");
        return ERR;
    }

    int set_yuv_channel_param(uint32_t width, uint32_t height, uint32_t fps) {
        /* 用于申请摄像头画面,根据参数不同,摄像头会为这套参数分配一个句柄,该句柄会返回指定配置的画面*/

        /*声明一个参数结构体对象*/
        SDC_YUV_CHANNEL_PARAM_S yuv_channel_param;
        /*将已经获得的channel id写入*/
        yuv_channel_param.channel = m_yuv_channel_id;
        yuv_channel_param.width = width;
        yuv_channel_param.height = height;
        yuv_channel_param.fps = fps;
        yuv_channel_param.on_off = 1;
        yuv_channel_param.format = SDC_YUV_420SP;


        yuv_channel_param.channel = m_yuv_channel_id;
        /*编写通讯请求*/
        int ret;
        char buf[4096];
        SDC_COMMON_HEAD_S *common_head;
        common_head = (SDC_COMMON_HEAD_S *) buf;
        common_head->version = SDC_VERSION;
        common_head->url = SDC_URL_YUV_CHANNEL;
        common_head->method = SDC_METHOD_UPDATE;
        common_head->head_length = sizeof(SDC_COMMON_HEAD_S);
        common_head->content_length = sizeof(SDC_YUV_CHANNEL_PARAM_S);

        /*因为参数已经保存在yuv_channel_param指针指向的地址了,
         * 因此这里就不把参数拷贝到buf中作为请求内容,而是拼接在一起使用writev函数写入句柄*/
        struct iovec iov_s[2] = {
                {.iov_base=common_head, .iov_len=common_head->head_length},
                {.iov_base=&yuv_channel_param, .iov_len=sizeof(SDC_YUV_CHANNEL_PARAM_S)},
        };
        ret = writev(m_fd_video, iov_s, 2);
        if (ret < 0) {
            DEBUG_LOG("ERR:writev fail yuv_channel_set,response:%d,url:%d,code:%d,method:%d\n",
                      common_head->response,
                      common_head->url,
                      common_head->code,
                      common_head->method);
            return ERR;
        }
        /*读取返回值,因为是设置参数,所以没有需要返回的内容参数,只需要读取返回code*/
        ret = read(m_fd_video, buf, sizeof(SDC_COMMON_HEAD_S));
        if (ret < 0 || common_head->code != SDC_CODE_200) {
            DEBUG_LOG("ERR:set_yuv_channel_params read fail response:%d,url:%d,code:%d, method:%d\n",
                      common_head->response,
                      common_head->url,
                      common_head->code,
                      common_head->method);
            return ERR;
        } else
            DEBUG_LOG("INFO:set_yuv_channel_params success.\n");
        return PAS;
    }

    int subscribe_video(uint32_t yuv_capacity = 25) {
        int ret, idx, yuv_data_num;
        char buf[2048] = {0};
        /*申请头*/
        SDC_COMMON_HEAD_S *common_head = (SDC_COMMON_HEAD_S *) buf;
        common_head->version = SDC_VERSION;
        common_head->url = SDC_URL_YUV_DATA;
        common_head->method = SDC_METHOD_GET;
        common_head->head_length = sizeof(*common_head);

        /*扩展头,设定通道的参数*/
        SDC_EXTEND_HEAD_S *extend_head = (SDC_EXTEND_HEAD_S *) (buf + common_head->head_length);
        extend_head->type = SDC_HEAD_YUV_CACHED_COUNT_MAX;
        extend_head->length = sizeof(SDC_EXTEND_HEAD_S);
        extend_head->reserve = yuv_capacity;//设置通道缓存数量
        common_head->head_length += extend_head->length;

        /*请求内容,告知请求订阅了多少通道*/
        UINT32 *channel = (UINT32 *) (buf + common_head->head_length);//对齐buf
        channel[0] = m_yuv_channel_id;
        common_head->content_length = sizeof(*channel);

        /*写入请求*/
        ret = write(m_fd_video, buf, common_head->head_length + common_head->content_length);
        if (ret < 0) {
            DEBUG_LOG("ERR:read_camara_data writing request fail.");
            return ERR;
        } else DEBUG_LOG("INFO:read_camara_data writing request success.");
        /*读取回复*/
        ret = read(m_fd_video, buf, sizeof(buf));
        if (ret < 0 || common_head->code != SDC_CODE_200) {
            DEBUG_LOG("ERR: subscripe yuv data failed, response url: %u, code: %u", common_head->url, common_head->code);
            return ERR;
        } else DEBUG_LOG("INFO: subscripe yuv data sucess.");
        return PAS;
    }

    int release_yuv(SDC_YUV_DATA_S *data) {
        /*释放数据,sdc有一套自己的内存管理方法,对于sdc自己调用的内存,必须申请sdc去释放.*/
        SDC_COMMON_HEAD_S head;
        head.version = SDC_VERSION;
        head.url = SDC_URL_YUV_DATA;
        head.method = SDC_METHOD_DELETE;
        head.content_length = sizeof(*data);
        head.head_length = sizeof(head);
        struct iovec iov[2] = {
                {.iov_base = &head, .iov_len = sizeof(head)},
                {.iov_base = data, .iov_len = sizeof(*data)}
        };
        //发送请求，写入服务文件，获得状态码
        int ret = writev(m_fd_video, iov, 2);
        if (ret < 0) {
            DEBUG_LOG("sdc_yuv_data release failed.");
            return ERR;
        }
        return PAS;
    }

    void* read_camera_data_run(){
        m_stop_reading=0;
        int ret,idx,yuv_data_num;
        char buf[4096];
        /*摄像头不会直接返回图像的具体数值,而是将保存了图像内容的内存地址以SDC_YUV_DATA_S的格式返回*/
        SDC_YUV_DATA_S *yuv_data;
        /*编写申请头*/
        SDC_COMMON_HEAD_S *common_head = (SDC_COMMON_HEAD_S *) buf;
        while(!m_stop_reading){
            usleep(1000);
            ret = read(m_fd_video, buf, sizeof(buf));
            if (ret < 0) {
                DEBUG_LOG("WARN:read yuv channel data from camara fail.response:%d,url:%d,code:%d, method:%d",
                       common_head->response,
                       common_head->url,
                       common_head->code,
                       common_head->method);
                continue;
            }
            /*订阅成功后SDC会往缓冲区不停写入视频帧,其中第一个响应没有内容*/
            switch (common_head->url) {
                /*当返回的报头类型为SDC_URL_YUV_DATA表示是视频数据*/
                case SDC_URL_YUV_DATA: {
                    // 跳过没有响应内容的报文
                    if (common_head->content_length != 0) {
                        /*sdc是可以一次性返回多帧数据的,通过报文内容的大小与SDC_YUV_DATA_S结构体做比得到具体的数据数量*/
                        yuv_data_num = common_head->content_length / sizeof(SDC_YUV_DATA_S);
                        yuv_data = (SDC_YUV_DATA_S *) (buf + common_head->head_length);
                        /*将数据放入队列中*/
                        for (idx = 0; idx < yuv_data_num; idx++) {
                            ret = m_video_queue->put(&yuv_data[idx]);
                            /*放入失败时(比如队列已满)则直接释放sdc上的内存*/
                            if (ret < 0) {
                                DEBUG_LOG("WARN:save yuv_data in queue failed, release data and continue.");
                                release_yuv(&yuv_data[idx]);
                            }
                        }
                    } else {
                        DEBUG_LOG("WARN: read data content_length = 0, code: %u.", common_head->code);
                    }
                    break;
                }
                default: {
                    DEBUG_LOG("get respost url is not SDC_URL_YUV_DATA");
                }
            }
        }
        DEBUG_LOG("INFO: Reading stopped.");
        return NULL ;
    }
    int get_data_from_queue(SDC_YUV_DATA_S* yuv_data,int duration_num){
        /*将保存在queue中的YUV_420SP格式的数据取出,并放入指针指向的内存空间中
         * yuv_data 用于保存数据的指针
         * duration_num 需要连续取多少帧,注意yuv_data的内从空间必须大于duration_num*sizeof(SDC_YUV_DATA_S)
         * */
        int ret;
        ret = m_video_queue->get(yuv_data,duration_num,&m_frame_idx);
        /*取数据是有可能失败的,比如队列中为空时,但是队列为空并不表示程序出错*/
        if (ret == QUEUE_EMPTY) {
            DEBUG_LOG("WARN: queue is empty, get data from queue failed,ret is: %i",ret);
        }
        return ret;
    }

};


int main() {
    ArrayQueue array_queue(25, sizeof(SDC_YUV_DATA_S));
    /* 构造函数会执行 注册服务 + 申请yuv_channel id + 导入队列*/
    VideoService video_service(&array_queue);
    //设定视频通道的参数
    video_service.set_yuv_channel_param(640, 480, 25);
    //订阅数据
    video_service.subscribe_video(25);
    //使用线程将订阅的数据不停的保存到数组队列中
    std::thread video_thread(&VideoService::read_camera_data_run,&video_service);
    //取出数据并释放
    int duration_num=1;//每次只取一帧数据
    SDC_YUV_DATA_S yuv_data[duration_num];
    int loop_condition=1,ret;

    while(loop_condition){
        ret=video_service.get_data_from_queue(yuv_data,duration_num);
        /*如果能正常取出数据,则打印数据的内存地址*/
        if (ret==PAS){
            DEBUG_LOG("addr_virt: %lu",yuv_data[0].frame.addr_virt);
            /*释放数据的内存空间,注意因为我们只取了一帧数据所以释放时是单个元素释放*/
            video_service.release_yuv(&yuv_data[0]);
        }
        //我们一秒取25帧,也就是40000微妙一帧,为了更好的观测到队列为空时的返回值,所以延迟30000微妙
        usleep(30000);

    }

    while (1){
        sleep(5);
    }

    printf("End\n");
}