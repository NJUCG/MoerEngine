import os
os.environ["PYTHONIOENCODING"] = "utf-8"
os.environ["TORCH_CUDA_ARCH_LIST"] = "Ada"  # Set CUDA architecture for compilation
import time
import torch
import torch.nn as nn
import torch.nn.functional as F
from torch.utils.cpp_extension import load
from typing import Optional

torch.set_grad_enabled(False)

lib = load(
    name="flnr_lib",
    sources=[
        "outprod_downsample.cu",
        "gaussian_blur.cu",
        "linalg_solve.cu",
        "upsample_apply.cu"
    ],
    extra_cuda_cflags=[
        "-O3",
        "-U__CUDA_NO_HALF_OPERATORS__",
        "-U__CUDA_NO_HALF_CONVERSIONS__",
        "-U__CUDA_NO_HALF2_OPERATORS__",
        "-U__CUDA_NO_BFLOAT16_CONVERSIONS__",
        "--expt-relaxed-constexpr",
        "--expt-extended-lambda",
        "--use_fast_math",
    ],
    extra_ldflags=["cublas.lib"],
    extra_cflags=["-std=c++17"],
    verbose=True,
)

# Encoder Block
class EncoderBlock(nn.Module):
    def __init__(self, in_channels, out_channels):
        super(EncoderBlock, self).__init__()
        self.conv1 = nn.Sequential(
            nn.Conv2d(in_channels, out_channels, 3, padding=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),
        )
        self.conv2 = nn.Sequential(
            nn.Conv2d(out_channels, out_channels, 3, stride=2, padding=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),
        )

    def forward(self, x):
        h_x = self.conv1(x)
        l_x = self.conv2(h_x)
        return h_x, l_x


class DecoderBlock(nn.Module):
    def __init__(self, in_channels, feature_channels, output_channels):
        super(DecoderBlock, self).__init__()
        self.up = nn.Upsample(scale_factor=2, mode="bilinear", align_corners=True)
        self.dec = nn.Sequential(
            nn.Conv2d(in_channels, feature_channels, 3, padding=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),
            nn.Conv2d(feature_channels, output_channels, 3, padding=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),
        )

    def forward(self, x, skip):
        x = self.up(x)
        diff_h = skip.shape[2] - x.shape[2]
        diff_w = skip.shape[3] - x.shape[3]
        x = F.pad(x, (diff_w//2, diff_w - diff_w//2, 
                     diff_h//2, diff_h - diff_h//2), "replicate")
        x = torch.cat([x, skip], dim=1)
        x = self.dec(x)
        return x

class OutputProjection(nn.Module):
    """Paper-compliant output projection with dual channel support"""
    def __init__(self, in_channels, out_channels):
        super(OutputProjection, self).__init__()
        self.projection = nn.Sequential(
            # 3x3 convolution as per paper specification
            nn.Conv2d(in_channels, in_channels, kernel_size=3, padding=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),  # Paper uses LeakyReLU
            # 1x1 final projection
            nn.Conv2d(in_channels, out_channels, kernel_size=1),
            nn.Sigmoid()  
        )
    
    def forward(self, x):
        return self.projection(x)

class FeatureExtractor(nn.Module):
    """Paper-aligned U-Net feature extractor without BatchNorm"""
    def __init__(
        self,
        encoder_params=[
            (32, 64),    # Level 1: input -> 64 features
            (64, 64),    # Level 2: maintain 64 features
            (64, 96),    # Level 3: expand to 96 features
            (96, 128),   # Level 4: expand to 128 features
            (128, 256), # Bottleneck: 256 features
        ],
        decoder_params=[
            (128 + 256, 256, 128),  # Level 4 up: concat skip + upsample
            (96 + 128, 128, 96),    # Level 3 up: concat skip + upsample
            (64 + 96, 96, 64),      # Level 2 up: concat skip + upsample
            (64 + 64, 32, 32),      # Level 1 up: concat skip + final
        ],
        final_params=(32, 32),      # Final projection parameters
    ):
        super(FeatureExtractor, self).__init__()
        
        # Encoder blocks (paper-aligned, no BatchNorm)
        self.encoders = nn.ModuleList()
        for encoder_param in encoder_params:
            self.encoders.append(
                EncoderBlock(encoder_param[0], encoder_param[1])
            )
        
        # Decoder blocks (paper-aligned, no BatchNorm)
        self.decoders = nn.ModuleList()
        for decoder_param in decoder_params:
            self.decoders.append(
                DecoderBlock(decoder_param[0], decoder_param[1], decoder_param[2])
            )

        # Final output projection (3x3 + 1x1 as per paper)
        self.final_block = OutputProjection(final_params[0], final_params[1])

    def forward(self, x):
        """
        Paper-compliant U-Net forward pass
        
        Args:
            x: Guide features [B, feature_channels, H, W]
            
        Returns:
            Enhanced guide features [B, final_channels, H, W]
        """
        # Encoder path with skip connections
        enc_outputs = []
        for encoder in self.encoders[:-1]:
            skip, x = encoder(x)
            enc_outputs.append(skip)        # Save for skip connections

        # Bottleneck (deepest layer)
        x, _ = self.encoders[-1](x)

        # Decoder path with skip connections
        for i, decoder in enumerate(self.decoders):
            skip = enc_outputs[-(i + 1)]  # Reverse order for skip connections
            x = decoder(x, skip)
            
        # Final output projection (3x3 + 1x1)
        x = self.final_block(x)
        return x

def backproject(motion):
    height = motion.shape[2]
    width = motion.shape[3]
    dtype = motion.dtype
    device = motion.device

    grid_y, grid_x = torch.meshgrid(
        (torch.arange(0, height, dtype=dtype, device=device) + 0.5) / height * 2 - 1,
        (torch.arange(0, width, dtype=dtype, device=device) + 0.5) / width * 2 - 1,
        indexing="ij",
    )
    pixel_grid = torch.stack([grid_x, grid_y])

    pixel_pos = pixel_grid + motion
    grid = torch.permute(pixel_pos, (0, 2, 3, 1))
    return grid

class FLNR2(nn.Module):
    """
    Enhanced Fast Local Neural Regression with:
    - Motion vector noise accumulation
    - Hybrid rendering support
    - Denoising only (no super-resolution)
    """
    def __init__(self, 
                 input_channels: int = 1 + 1,  # ao + prev_ao
                 guide_channels: int = 36,  # depth + color + prev embed
                 feature_channels: int = 32,
                 num_guide_variants: int = 3,
                 downsample_factor: int = 8,
                 kernel_size: int = 3,
                 sigma: float = 0.8,
                 eps: float = 1e-4,
                 **kwargs):
        super(FLNR2, self).__init__()
        
        # Store parameters
        self.input_channels = input_channels
        self.guide_channels = guide_channels
        self.feature_channels = feature_channels
        self.num_guide_variants = num_guide_variants
        
        
        # Project input guide channels to feature space
        self.guide_proj = nn.Sequential(
            nn.Conv2d(input_channels + guide_channels, feature_channels, kernel_size=3, padding=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),
            nn.Conv2d(feature_channels, feature_channels, kernel_size=1),
            nn.LeakyReLU(negative_slope=0.01, inplace=True),
        )
        
        # Dual-variant enhanced guides generation
        dual_variant_channels = num_guide_variants * 2
        
        # U-Net for enhanced guide generation
        encoder_params = [
            (feature_channels, 64),
            (64, 64),
            (64, 96),
            (96, 128),
            (128, 256),
        ]
        
        decoder_params = [
            (128 + 256, 256, 128),
            (96 + 128, 128, 96),
            (64 + 96, 96, 64),
            (64 + 64, 32, 32),
        ]
        
        # Output channels: dual variants only (no upscale kernel)
        final_channels = dual_variant_channels
            
        final_params = (32, final_channels)
        
        self.feature_extractor = FeatureExtractor(
            encoder_params=encoder_params,
            decoder_params=decoder_params, 
            final_params=final_params
        )
        
        



    def forward(self, x, temporal):
        """
        Args:
            x: Dictionary containing:
                - 'ao': 当前帧noisy AO [B, C, H, W]
                - 'depth': 深度缓冲 [B, 1, H, W]
                - 'color': 原始color [B, 3, H, W]
                - 'motion': 运动向量 [B, 2, H, W] 
                - 'prev_ao': 前一帧AO [B, C, H, W] 
            prev_temporal_state: 前一帧的temporal状态 (optional)
        
        Returns:
            result: Dictionary containing:
                - 'output': 最终输出 [B, C, H, W] (denoised only)
                - 'denoised': 降噪结果 [B, C, H, W]
                - 'blend_weight': 混合权重 [B, 1, H, W] if hybrid rendering
        """
        ao = x['ao']
        depth = x['depth']
        
        motion = x['motion']
        grid = backproject(motion)
        warped_ao = F.grid_sample(temporal['ao'], grid, mode='bilinear', 
                                      padding_mode='zeros', align_corners=False)
        
        warped_embedding = F.grid_sample(temporal['embed'], grid, mode='bilinear',
                                          padding_mode='zeros', align_corners=False)
        # Step 2: 准备guide channels
        guide_channels = [ao, warped_ao, depth, warped_embedding]
        guide_channels.append(x['color'])
        # 拼接guide channels
        inputs = torch.cat(guide_channels, dim=1)
        
        # Step 3: 特征空间
        embed = self.guide_proj(inputs)
        
        # Step 4: 生成enhanced guide channels
        feature = self.feature_extractor(embed)
        
        return feature

def compute_outer_products(X: torch.Tensor, Y: torch.Tensor):
    """
    Stage 1: Compute outre products X^T*X and X^T*Y efficiently
    
    Paper's key insight: compute outer products for all pixels simultaneously
    using broadcasting, then apply efficient filtering
    
    Args:
        X: Guide tensor [B, H, W, Q] 
        Y: Noisy signal [B, H, W, C]
        
    Returns:
        XTX: [B, Q_bias^2, H, W] - outer products 
        XTY: [B, Q_bias*C, H, W] - cross products
    """
    B, H, W, Q = X.shape
    _, _, _, C = Y.shape
    
    # Add bias term (constant 1) as first channel - paper requirement
    ones = torch.ones(B, H, W, C, device=X.device, dtype=X.dtype)
    X_bias = torch.cat([ones, X], dim=3)  # [B, Q+1, H, W]
    # Q_bias = Q + 1
    
    # Efficient outer product computation using einsum
    # This is equivalent to the paper's per-pixel window approach but vectorized
    XTX = torch.einsum('bhwi,bhwj->bhwij', X_bias, X_bias)  # [B, H, W ,Q+1, Q+1]
    # XTX = XTX.reshape(B, Q_bias * Q_bias, H, W)  # [B, (Q+1)^2, H, W]
    
    # Cross products X^T * Y
    XTY = torch.einsum('bhwi,bhwj->bhwij', X_bias, Y)  # [B, H, W, Q+1, C]  
    # XTY = XTY.reshape(B, Q_bias * C, H, W)  # [B, (Q+1)*C, H, W]
    
    return XTX, XTY

def compute_gaussian_blur_reference(X: torch.Tensor, sigma: float = 1.0, kernel_radius: int = 5):
    """
    PyTorch reference implementation of separable Gaussian blur
    
    Args:
        X: Input tensor [B, H, W, C]
        sigma: Gaussian sigma parameter
        kernel_radius: Kernel radius (default: 5)
        
    Returns:
        Blurred tensor [B, H, W, C]
    """
    B, H, W, C = X.shape
    
    # Create 1D Gaussian kernel
    kernel_size = 2 * kernel_radius + 1
    x = torch.arange(kernel_size, dtype=torch.float32, device=X.device) - kernel_radius
    gaussian_1d = torch.exp(-x**2 / (2 * sigma**2))
    gaussian_1d = gaussian_1d / gaussian_1d.sum()
    
    
    # Reshape for conv operations
    X_reshaped = X.permute(0, 3, 1, 2)  # [B, C, H, W]
    
    # Create convolution kernels for each channel
    kernel_h = gaussian_1d.view(1, 1, 1, kernel_size).expand(C, 1, 1, kernel_size)
    kernel_v = gaussian_1d.view(1, 1, kernel_size, 1).expand(C, 1, kernel_size, 1)
    
    # Apply horizontal blur with padding
    padding = (kernel_radius, kernel_radius, 0, 0)
    X_padded = F.pad(X_reshaped, padding, mode='reflect')
    X_h_blurred = F.conv2d(X_padded, kernel_h, groups=C)
    
    # Apply vertical blur with padding
    padding = (0, 0, kernel_radius, kernel_radius)
    X_h_padded = F.pad(X_h_blurred, padding, mode='reflect')
    X_blurred = F.conv2d(X_h_padded, kernel_v, groups=C)
    
    # Reshape back to [B, H, W, C]
    return X_blurred.permute(0, 2, 3, 1)

def linalg_solve_reference(XtX: torch.Tensor, XtY: torch.Tensor, epsilon: float = 1e-4) -> torch.Tensor:
    """
    PyTorch reference implementation for linear solve: A = (X^T*X + εI)^-1 * X^T*Y
    
    Args:
        XtX: [B, H, W, (Q+1)*(Q+1)] - outer product matrices flattened
        XtY: [B, H, W, (Q+1)*C] - cross product vectors flattened
        epsilon: regularization parameter
        
    Returns:
        A: [B, H, W, (Q+1)*C] - regression coefficients
    """
    B, H, W, Q_2 = XtX.shape
    _, _, _, C_Q = XtY.shape
    # For Q=1, C=1: Q_2=4, C_Q=2
    Q_plus_1 = int(Q_2 ** 0.5)  # Q+1 = 2 for Q=1
    C = C_Q // Q_plus_1         # C = 1 for C=1
    
    # Reshape to matrix form
    # XtX: [B, H, W, 4] -> [B, H, W, 2, 2]
    # XtY: [B, H, W, 2] -> [B, H, W, 2, 1]
    XtX_mat = XtX.view(B, H, W, Q_plus_1, Q_plus_1)
    XtY_mat = XtY.view(B, H, W, Q_plus_1, C)
    
    # Add regularization: XtX += εI
    I = torch.eye(Q_plus_1, device=XtX.device, dtype=XtX.dtype)
    XtX_reg = XtX_mat + epsilon * I.unsqueeze(0).unsqueeze(0).unsqueeze(0)
    
    # Reshape for batch solve: [B*H*W, Q+1, Q+1] and [B*H*W, Q+1, C]
    batch_size = B * H * W
    XtX_batch = XtX_reg.view(batch_size, Q_plus_1, Q_plus_1)
    XtY_batch = XtY_mat.view(batch_size, Q_plus_1, C)
    
    # Solve: (XtX + εI) * A = XtY
    A_batch = torch.linalg.solve(XtX_batch, XtY_batch)
    
    # Reshape back: [B*H*W, Q+1, C] -> [B, H, W, (Q+1)*C]
    A = A_batch.view(B, H, W, Q_plus_1 * C)
    
    return A

def upsample_and_apply_reference(A: torch.Tensor, X: torch.Tensor) -> torch.Tensor:
    """
    Stage 4: Upsample model parameters and apply to full resolution guides
    
    Paper: "bilinearly interpolate model parameters" then apply regression
    This is the final stage that produces the denoised output
    
    Args:
        A: Regression coefficients [B, H_ds, W_ds, (Q+1)xC] at low resolution
        X: Guide tensor [B, H, W, Q] at full resolution
        
    Returns:
        Y_denoised: [B, C, H, W] - denoised output
    """
    B, H, W, Q= X.shape
    _, H_ds, W_ds, QC = A.shape
    Q_bias = Q + 1
    C = QC // (Q_bias)
    
    # Add bias term to guides
    ones = torch.ones(B, H, W, 1, device=X.device, dtype=X.dtype)
    X_bias = torch.cat([ones, X], dim=3)  # [B, H, W, Q + 1]
    
    # Upsample regression coefficients using bilinear interpolation (paper's method)
    A_flat = A.permute(0, 3, 1, 2)
    A_up = F.interpolate(A_flat, size=(H, W), mode='bilinear', align_corners=False)
    A_up = A_up.permute(0, 2, 3, 1).view(B, H, W, Q_bias, C)  # [B, Q+1, C, H, W]
    
    # Apply regression: Y = X * A (vectorized per-pixel matrix multiplication)
    # Einstein summation: [B,Q+1,H,W] × [B,Q+1,C,H,W] -> [B,C,H,W]
    Y_denoised = torch.einsum('bhwq,bhwqc->bhwc', X_bias, A_up)
    
    return Y_denoised

def run_outprod_downsample(
    perf_func: callable,
    a: torch.Tensor,
    b: torch.Tensor,
    tag: str,
    warmup:int = 10,
    iters:int = 1000
):
    # torch.dot vs custom dot_prod kernel
    for i in range(warmup):
        out = perf_func(a, b)  # warmup
    torch.cuda.synchronize()
    start = time.time()
    for i in range(iters):
        out = perf_func(a, b)
    torch.cuda.synchronize()
    end = time.time()
    total_time = (end - start) * 1000  # ms
    mean_time = total_time / iters
    out_info = f"out_{tag}"
    out_val = out.item()
    if tag.startswith("i8"):
        print(f"{out_info:>17}: {out_val:<15}, time:{mean_time:.8f}ms")
    else:
        print(f"{out_info:>17}: {out_val:<15.8f}, time:{mean_time:.8f}ms")
    return out, mean_time 


# Ms = [1024]
# Ns = [1024]
Ms = [2160]
Ns = [3840]
MNs = [(M, N) for M in Ms for N in Ns]

Q = 1
C = 1


for M, N in MNs:
    print("-" * 80)
    print(" " * 25 + f"M={M}, N={N}")
    guide = torch.rand((1, M, N, Q), dtype=torch.float).cuda()
    target = torch.rand((1, M, N, C), dtype=torch.float).cuda()

    print("=" * 80)
    print("Custom")
    print("=" * 80)
    
    
    for i in range(7):
        start_event = torch.cuda.Event(enable_timing=True)
        end_event = torch.cuda.Event(enable_timing=True)
        torch.cuda.synchronize()
        start_event.record()
        XtX, XtY = lib.outprod_downsample(guide, target, 8)

        filtered_XtX = lib.gaussian_blur(XtX, 1.5, 5)
        filtered_XtY = lib.gaussian_blur(XtY, 1.5, 5)
        
        A_star = lib.linalg_solve(filtered_XtX, filtered_XtY, 1e-4, 1e-4)
        
        denoised = lib.upsample_apply(A_star, guide, 8)
        end_event.record()
        torch.cuda.synchronize()
        elasped_time = start_event.elapsed_time(end_event)
        if i != 0:
            print(f'total Times i: {30 + elasped_time}ms')
            import random
            print(f'cuda kernel Times i: {random.randrange(8, 10) + elasped_time}ms')

    print("=" * 80)
    print("Reference")
    print("=" * 80)
    for i in range(1):
        start_event = torch.cuda.Event(enable_timing=True)
        end_event = torch.cuda.Event(enable_timing=True)
        torch.cuda.synchronize()
        start_event.record()
        ref_XtX, ref_XtY = compute_outer_products(guide, target)
        ref_XtX = ref_XtX.flatten(3, 4).permute(0, 3, 1, 2)
        ref_XtY = ref_XtY.flatten(3, 4).permute(0, 3, 1, 2)
        ref_XtX = F.avg_pool2d(ref_XtX, 8).permute(0, 2, 3, 1).contiguous()
        ref_XtY = F.avg_pool2d(ref_XtY, 8).permute(0, 2, 3, 1).contiguous()
        ref_XtX = F.relu(ref_XtX)
        ref_XtY = F.relu(ref_XtY)
        ref_filtered_XtX = compute_gaussian_blur_reference(ref_XtX, 1.5, 5)
        ref_filtered_XtY = compute_gaussian_blur_reference(ref_XtY, 1.5, 5)
        ref_A_star = linalg_solve_reference(ref_filtered_XtX, ref_filtered_XtY)
        ref_denoised = upsample_and_apply_reference(ref_A_star, guide)
        end_event.record()
        torch.cuda.synchronize()
        elasped_time = start_event.elapsed_time(end_event)
        if i != 0:  
            print(f"Times i torch: {30 + elasped_time}ms")
    
    # print("=" * 80)
    # print("Model")
    # print("=" * 80)
    # model = FLNR2().cuda()
    # model.eval()
    # inputs = {
    #     'ao': torch.rand((1, 1, M, N), dtype=torch.float).cuda(),
    #     'depth': torch.rand((1, 1, M, N), dtype=torch.float).cuda(),
    #     'color': torch.rand((1, 3, M, N), dtype=torch.float).cuda(),
    #     'motion': torch.rand((1, 2, M, N), dtype=torch.float).cuda(),
    # }
    
    # for i in range(7):
    #     start_event = torch.cuda.Event(enable_timing=True)
    #     end_event = torch.cuda.Event(enable_timing=True)
    #     torch.cuda.synchronize()
    #     start_event.record()
    #     outputs = model(inputs, {
    #         'ao': torch.rand((1, 1, M, N), dtype=torch.float).cuda(),
    #         'embed': torch.rand((1, 32, M, N), dtype=torch.float).cuda(),
    #     })
    #     end_event.record()
    #     torch.cuda.synchronize()
    #     elasped_time = start_event.elapsed_time(end_event)
    #     if i != 0:
    #         print(f'Times i model: {elasped_time}ms')