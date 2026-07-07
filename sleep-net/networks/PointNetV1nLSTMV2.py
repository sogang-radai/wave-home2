"""SleepNet with shared PointNet encoder and uni-directional LSTM temporal heads.

Temporal pooling uses the mean over the sequence (not max-pool) to reduce
spike sensitivity on toss / bed logits compared to TemporalCNNV1 + AdaptiveMaxPool.
"""

from __future__ import annotations

from typing import Optional, Tuple

import torch
import torch.nn as nn

EMBEDDING_DIM: int = 256
SEQUENCE_LENGTH_BED: int = 100
SEQUENCE_LENGTH_TOSS: int = 40
CLASS_COUNT_BED: int = 3
CLASS_COUNT_TOSS: int = 3

class PointNetV1(nn.Module):
    def __init__(self):
        super(PointNetV1, self).__init__()

        self.mlp1 = nn.Sequential(
            nn.Conv1d(in_channels=5, out_channels=32, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=32, out_channels=64, kernel_size=1)
        )
        self.bn1 = nn.BatchNorm1d(64)
        self.relu1 = nn.ReLU(inplace=True)

        self.mlp2 = nn.Sequential(
            nn.Conv1d(in_channels=64, out_channels=64, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=64, out_channels=128, kernel_size=1),
            nn.ReLU(inplace=True),
            nn.Conv1d(in_channels=128, out_channels=EMBEDDING_DIM, kernel_size=1)
        )
        self.bn2 = nn.BatchNorm1d(EMBEDDING_DIM)
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
            x = x * mask_float

        x = torch.max(x, 2)[0]

        return x


class TemporalLSTMV1(nn.Module):
    """Uni-directional LSTM + temporal mean pool + linear classifier."""

    def __init__(
        self,
        num_classes: int,
        input_size: int = EMBEDDING_DIM,
        hidden_size: int = 128,
        num_layers: int = 2,
        dropout: float = 0.3,
    ) -> None:
        super().__init__()
        self.lstm = nn.LSTM(
            input_size=input_size,
            hidden_size=hidden_size,
            num_layers=num_layers,
            batch_first=True,
            bidirectional=False,
            dropout=dropout if num_layers > 1 else 0.0,
        )
        self.dropout = nn.Dropout(dropout)
        self.fc = nn.Linear(hidden_size, num_classes)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        # x: (B, S, E)
        outputs, _ = self.lstm(x)
        pooled = outputs.mean(dim=1)
        return self.fc(self.dropout(pooled))


class SleepNet(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.frame_encoder = PointNetV1()
        self.head_status = TemporalLSTMV1(num_classes=CLASS_COUNT_BED)
        self.head_toss = TemporalLSTMV1(num_classes=CLASS_COUNT_TOSS)

    def encode_frames(
        self,
        points: torch.Tensor,
        mask: Optional[torch.Tensor] = None,
    ) -> torch.Tensor:
        return self.frame_encoder(points, mask)

    def classify_embeddings(
        self,
        frame_embeddings: torch.Tensor,
    ) -> Tuple[torch.Tensor, torch.Tensor]:
        return self.head_status(frame_embeddings), self.head_toss(frame_embeddings)

    def forward(
        self,
        points: torch.Tensor,
        mask: torch.Tensor,
    ) -> Tuple[torch.Tensor, torch.Tensor]:
        batch_size, seq_len, max_points, features = points.shape

        flat_points = points.view(batch_size * seq_len, max_points, features).transpose(1, 2)

        if mask is not None:
            flat_mask = mask.view(batch_size * seq_len, max_points)
        else:
            flat_mask = None

        frame_embeddings = self.encode_frames(flat_points, flat_mask)
        frame_embeddings = frame_embeddings.view(batch_size, seq_len, -1)

        return self.classify_embeddings(frame_embeddings)
