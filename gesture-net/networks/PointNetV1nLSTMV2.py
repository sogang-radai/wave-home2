from typing import Optional

import torch
import torch.nn as nn

SEQUENCE_LENGTH: int = 48
GESTURE_COUNT: int = 14

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

class TemporalLSTMHeadV1(nn.Module):
    def __init__(self, input_size=256, hidden_size=128):
        super(TemporalLSTMHeadV1, self).__init__()
        self.lstm = nn.LSTM(input_size=input_size, hidden_size=hidden_size, num_layers=1, batch_first=True)
        self.fc = nn.Linear(hidden_size, GESTURE_COUNT)

    def forward(self, x):
        lstm_out, (h_n, c_n) = self.lstm(x)
        last_step_out = lstm_out[:, -1, :]
        logits = self.fc(last_step_out)

        return logits

class GestureClassifierV2(nn.Module):
    def __init__(self) -> None:
        super().__init__()
        self.frame_encoder = PointNetFrameEncoderV1()
        self.temporal_head = TemporalLSTMHeadV1()

    def encode_frames(self, points: torch.Tensor, mask: Optional[torch.Tensor] = None) -> torch.Tensor:
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