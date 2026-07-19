from typing import Optional

import torch
import torch.nn as nn
import torch.nn.functional as F

SEQUENCE_LENGTH: int = 60
GESTURE_COUNT: int = 6

class PointNetFrameEncoderV1(nn.Module):
    def __init__(self):
        super(PointNetFrameEncoderV1, self).__init__()

        # MLP1 (n x 5 -> n x 32 -> n x 64)
        self.mlp1 = nn.Sequential(
            nn.Conv1d(in_channels=5, out_channels=32, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=32, out_channels=64, kernel_size=1)
        )
        self.bn1 = nn.BatchNorm1d(64)
        self.relu1 = nn.ReLU(inplace=True)

        # MLP2 (n x 64 -> n x 64 -> n x 128 -> n x 256)
        self.mlp2 = nn.Sequential(
            nn.Conv1d(in_channels=64, out_channels=64, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=64, out_channels=128, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=128, out_channels=256, kernel_size=1)
        )
        self.bn2 = nn.BatchNorm1d(256)
        self.relu2 = nn.ReLU(inplace=True)

    def forward(self, x, mask: Optional[torch.Tensor] = None):
        x = self.mlp1(x)
        x = self.bn1(x)
        x = self.relu1(x)

        x = self.mlp2(x)
        x = self.bn2(x)
        x = self.relu2(x)

        if mask is not None:
            mask_float = mask.unsqueeze(1).float()
            x = x * mask_float + (1.0 - mask_float) * -1e9

        x = torch.max(x, 2)[0]

        return x

class TemporalBlock(nn.Module):
    def __init__(self, channels, dilation=1, dropout=0.3):
        super().__init__()
        padding = dilation

        self.net = nn.Sequential(
            nn.Conv1d(channels, channels, kernel_size=3, padding=padding, dilation=dilation, bias=False),
            nn.BatchNorm1d(channels),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout),
            nn.Conv1d(channels, channels, kernel_size=3, padding=padding, dilation=dilation, bias=False),
            nn.BatchNorm1d(channels),
        )

        self.relu = nn.ReLU(inplace=True)

    def forward(self, x):
        return self.relu(x + self.net(x))


class CNNV2(nn.Module):
    def __init__(self, input_size=256, hidden_size=96, num_classes=6, dropout=0.3):
        super().__init__()

        self.proj = nn.Sequential(
            nn.Conv1d(input_size, hidden_size, kernel_size=1, bias=False),
            nn.BatchNorm1d(hidden_size),
            nn.ReLU(inplace=True),
        )

        self.blocks = nn.Sequential(
            TemporalBlock(hidden_size, dilation=1, dropout=dropout),
            TemporalBlock(hidden_size, dilation=2, dropout=dropout),
            TemporalBlock(hidden_size, dilation=4, dropout=dropout),
        )

        self.fc = nn.Sequential(
            nn.Linear(hidden_size * 2, hidden_size),
            nn.ReLU(inplace=True),
            nn.Dropout(dropout),
            nn.Linear(hidden_size, num_classes),
        )

    def forward(self, x):
        x = x.transpose(1, 2)
        x = self.proj(x)
        x = self.blocks(x)

        max_pool = torch.max(x, dim=2)[0]
        avg_pool = torch.mean(x, dim=2)

        x = torch.cat([max_pool, avg_pool], dim=1)
        logits = self.fc(x)

        return logits
    
class GestureClassifierV1(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.frame_encoder = PointNetFrameEncoderV1() # V1 or V2
        self.temporal_head = CNNV1() # CNNV1 or LSTMV1 or TransformerV1

    def encode_frames(self, points: torch.Tensor, mask: Optional[torch.Tensor] = None) -> torch.Tensor:
        # encoder에 mask도 같이 넘겨줍니다.
        return self.frame_encoder(points, mask)

    def classify_embeddings(self, frame_embeddings: torch.Tensor) -> torch.Tensor:
        return self.temporal_head(frame_embeddings)

    def forward(self, points: torch.Tensor, mask: torch.Tensor) -> torch.Tensor:
        batch_size, seq_len, max_points, features = points.shape

        flat_points = points.view(batch_size * seq_len, max_points, features)
        flat_points = flat_points.transpose(1, 2)

        if mask is not None:
            flat_mask = mask.view(batch_size * seq_len, max_points)
        else:
            flat_mask = None

        frame_embeddings = self.encode_frames(flat_points, flat_mask)
        frame_embeddings = frame_embeddings.view(batch_size, seq_len, -1)

        logits = self.classify_embeddings(frame_embeddings)

        return logits