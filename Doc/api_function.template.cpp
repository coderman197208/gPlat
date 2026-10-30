// 客户端 API 函数模板
// 用途: 添加新的客户端 API
// 使用: 复制此模板，替换 function_name 为实际函数名

// 1. 在 include/higplat.h 声明 API
/*
extern "C" bool function_name(
    int sockfd,
    const char* param1,
    void* param2,
    int param3,
    unsigned int* error
);
*/

// 2. 在 higplat/net_client.cpp 实现 API（辅助函数都在该文件的匿名命名空间里）
//    - 每个导出函数开头声明 AutoErrorCheck，错误码非 0 时恰好上报一次；内部辅助函数里不要声明
//    - transact 发送包头（自动设置 bodysize）和包体，再收响应包头；失败时已 shutdown 连接并置 CONNECTION 类错误
//    - 响应无包体用 recv_status（或要求回显 id 的 recv_ack），有包体用 recv_body（放不下时读掉丢弃，连接保留）
//    - 不得 close(sockfd)：fd 归调用方
extern "C" bool function_name(int sockfd, const char* tagname, void* value, int actsize, unsigned int* error)
{
    if (error == nullptr)
        return false;

    AutoErrorCheck _checker(error, __func__);
    if (!tagname || !value || actsize <= 0) {
        *error = ERROR_INVALID_PARAMETER;
        return false;
    }
    if (actsize > MAXMSGLEN) {
        *error = ERROR_PARAMETER_SIZE;
        return false;
    }

    // 请求：MSGHEAD + actsize 字节包体
    MSGHEAD head = make_board_head(NEW_COMMAND_NAME, tagname);  // 非 Board 请求用 make_head + copy_name
    head.datasize = actsize;
    if (!transact(sockfd, head, value, actsize, error))
        return false;

    // 响应不带包体时：
    return recv_status(sockfd, head, error);

    // 响应带包体时（读入调用方缓冲区 out，容量 outsize）：
    // *error = head.error;
    // return *error == 0 && recv_body(sockfd, head, out, outsize, error);
}

// 3. 使用示例
/*
int main() {
    unsigned int error = 0;
    int sockfd = connectgplat("127.0.0.1", 8777);

    if (sockfd < 0) {
        printf("Connect failed\n");
        return -1;
    }

    bool result = function_name(sockfd, "param1", data, dataSize, &error);
    if (!result) {
        printf("Operation failed, error: %u\n", error);
    }

    disconnectgplat(sockfd);
    return 0;
}
*/
