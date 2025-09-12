package org.flexlb.service.grpc;

import lombok.Getter;
import org.flexlb.dao.master.WorkerStatus;
import org.flexlb.dao.route.RoleType;
import org.flexlb.domain.balance.WhaleMasterConfig;
import org.flexlb.engine.grpc.EngineGrpcClient;
import org.flexlb.engine.grpc.EngineRpcService;
import org.flexlb.service.config.ConfigService;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

/**
 * Engine gRPC Service for worker status queries
 */
@Component
public class EngineGrpcService {

    private static final Logger logger = LoggerFactory.getLogger("syncLogger");

    @Getter
    private final EngineGrpcClient engineGrpcClient;
    private final WhaleMasterConfig whaleMasterConfig;

    public EngineGrpcService(ConfigService configService, EngineGrpcClient engineGrpcClient) {
        this.whaleMasterConfig = configService.loadBalanceConfig();
        this.engineGrpcClient = engineGrpcClient;
    }

    /**
     * Get worker status via gRPC
     *
     * @param ip                  worker IP
     * @param grpcPort            worker gRPC port
     * @param finishedTaskVersion finished task version
     * @return CompletableFuture of WorkerStatusPB
     */
    public EngineRpcService.WorkerStatusPB getWorkerStatus(String ip, int grpcPort, long finishedTaskVersion, long requestTimeoutMs) {
        if (engineGrpcClient == null) {
            throw new RuntimeException("EngineGrpcService not initialized");
        }

        EngineRpcService.StatusVersionPB request = EngineRpcService.StatusVersionPB.newBuilder()
                .setLatestFinishedVersion(finishedTaskVersion)
                .build();

        return engineGrpcClient.getWorkerStatus(ip, grpcPort, request, requestTimeoutMs);
    }

    /**
     * Get cache status via gRPC
     *
     * @param ip           worker IP
     * @param grpcPort     worker gRPC port
     * @param workerStatus worker status
     * @param cacheVersion cache version for status check
     * @return CompletableFuture of CacheStatusPB
     */
    public EngineRpcService.CacheStatusPB getCacheStatus(
        String ip, int grpcPort, WorkerStatus workerStatus, long cacheVersion, long requestTimeoutMs) {

        if (engineGrpcClient == null) {
            throw new RuntimeException("EngineGrpcService not initialized");
        }
        boolean isPrefill = RoleType.PREFILL.matches(workerStatus.getRole());
        EngineRpcService.CacheVersionPB request = EngineRpcService.CacheVersionPB.newBuilder()
                .setLatestCacheVersion((int) cacheVersion)
                .setNeedCacheKeys(isPrefill)
                .build();
        logger.info("Get cache status Request: {}, cacheVersion: {}, needCacheKeys: {}", ip, cacheVersion, isPrefill);
        return engineGrpcClient.getCacheStatus(ip, grpcPort, request, requestTimeoutMs);
    }

    public boolean isEngineStatusEnabled() {
        return whaleMasterConfig.isEnableGrpcEngineStatus();
    }

    public boolean isCacheStatusEnabled() {
        return whaleMasterConfig.isEnableGrpcCacheStatus();
    }
}